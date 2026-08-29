/*
 * test-client-rules.c - The two rules both graphical clients apply
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * An unread count and an alert tier are decided identically by the GTK
 * client and the web client, and were implemented twice before this --
 * which is the shape of drift `make parity` exists to catch and cannot
 * see, because neither rule sends a frame or answers a command.  Pure
 * functions, so every branch is exercised without a window, a browser or
 * a daemon.
 */

#include "clawtilla.h"

#include <glib.h>
#include <math.h>

/* Anything after this instant is live rather than replayed. */
#define CONNECTED_AT G_GINT64_CONSTANT(1000000)
#define LIVE         (CONNECTED_AT + 1000)
#define REPLAYED     (CONNECTED_AT - 1000)

static void
test_a_message_elsewhere_counts(void)
{
    g_assert_true(clawt_unread_should_count("dm:beta:user", "dm:alpha:user",
                                            "beta", LIVE, CONNECTED_AT));
}

/*
 * A conversation on screen never accrues one, whatever the scroll
 * position: that case belongs to the transcript's "New messages" rule,
 * which deliberately carries no number.  The two must never fire for the
 * same message.
 */
static void
test_the_room_on_screen_does_not(void)
{
    g_assert_false(clawt_unread_should_count("dm:alpha:user",
                                             "dm:alpha:user", "alpha", LIVE,
                                             CONNECTED_AT));
}

/* Nothing on screen at all -- a settings page -- still counts. */
static void
test_no_room_on_screen_still_counts(void)
{
    g_assert_true(clawt_unread_should_count("dm:alpha:user", NULL, "alpha",
                                            LIVE, CONNECTED_AT));
}

static void
test_your_own_message_does_not(void)
{
    g_assert_false(clawt_unread_should_count("dm:beta:user", "dm:alpha:user",
                                             "user", LIVE, CONNECTED_AT));
}

/*
 * A client subscribes from cursor 0 and the daemon replays its recent
 * events, so the first thing a fresh window receives is everything that
 * just happened -- possibly read in the previous session.  Counting
 * those opens a window already showing a number for a conversation
 * nobody has touched.
 */
static void
test_a_replayed_message_does_not(void)
{
    g_assert_false(clawt_unread_should_count("dm:beta:user", "dm:alpha:user",
                                             "beta", REPLAYED,
                                             CONNECTED_AT));
}

/* An event with no timestamp is taken at face value rather than dropped. */
static void
test_an_undated_message_counts(void)
{
    g_assert_true(clawt_unread_should_count("dm:beta:user", "dm:alpha:user",
                                            "beta", 0, CONNECTED_AT));
}

static void
test_nothing_without_a_room_or_a_sender(void)
{
    g_assert_false(clawt_unread_should_count(NULL, NULL, "beta", LIVE,
                                             CONNECTED_AT));
    g_assert_false(clawt_unread_should_count("dm:beta:user", NULL, NULL,
                                             LIVE, CONNECTED_AT));
}

/* ── Following the live edge ─────────────────────────────────────── */

/*
 * The predicate the whole follow behaviour turns on.
 *
 * A client refuses to move the view when it is false, and both unread
 * affordances -- the pill and the rule in the transcript -- are driven by
 * the edge where it changes.  It was thirty-two pixels of arithmetic
 * inside a signal handler and could not be tested at all; it is a pure
 * function now, and these are the cases nobody drives by hand.
 */
static void
test_at_the_bottom_follows(void)
{
    /* Exactly there. */
    g_assert_true(clawt_transcript_is_at_bottom(1000.0, 1600.0, 600.0));

    /* Within the tolerance, which exists because a scrolled window
     * rarely lands on an exact value. */
    g_assert_true(clawt_transcript_is_at_bottom(969.0, 1600.0, 600.0));

    /* Past the end, which GTK clamps but which must not read as away. */
    g_assert_true(clawt_transcript_is_at_bottom(1200.0, 1600.0, 600.0));
}

static void
test_away_from_the_bottom_does_not(void)
{
    g_assert_false(clawt_transcript_is_at_bottom(0.0, 1600.0, 600.0));
    g_assert_false(clawt_transcript_is_at_bottom(900.0, 1600.0, 600.0));
}

/*
 * The boundary itself, asserted on both sides.
 *
 * A tolerance nobody tests at its edge is a tolerance that quietly
 * becomes off-by-one, and every pixel of it is a pixel of message a new
 * arrival can push off the bottom without the client noticing it stopped
 * following.
 */
static void
test_the_tolerance_boundary(void)
{
    gdouble bottom = 1000.0;

    /* One pixel inside. */
    g_assert_true(clawt_transcript_is_at_bottom(
        bottom - (CLAWT_TRANSCRIPT_FOLLOW_TOLERANCE - 1.0), 1600.0, 600.0));

    /* Exactly the tolerance is *not* inside: the test is strictly less. */
    g_assert_false(clawt_transcript_is_at_bottom(
        bottom - CLAWT_TRANSCRIPT_FOLLOW_TOLERANCE, 1600.0, 600.0));
}

/*
 * A transcript shorter than its viewport is at the bottom by definition:
 * there is nowhere else to be.
 *
 * The arithmetic happened to give the right answer here already -- a
 * negative bottom is still less than the tolerance -- so the explicit
 * guard is not a fix.  It is here because "shorter than the viewport"
 * being correct by accident is the kind of thing a later edit breaks
 * without noticing, and these three cases are what would notice.
 */
static void
test_a_short_transcript_is_always_at_the_bottom(void)
{
    g_assert_true(clawt_transcript_is_at_bottom(0.0, 200.0, 600.0));
    g_assert_true(clawt_transcript_is_at_bottom(0.0, 0.0, 0.0));
    g_assert_true(clawt_transcript_is_at_bottom(0.0, 600.0, 600.0));
}

/* ── Alert tiers ─────────────────────────────────────────────────── */

static ClawtAlertTier
tier_of(const gchar *kind, const gchar *detail_key, const gchar *detail)
{
    g_autoptr(ClawtEvent) event = clawt_event_new(kind, "alpha");

    if (detail_key != NULL)
        clawt_event_set_detail(event, detail_key, detail);

    return clawt_alert_tier_for_event(event);
}

/*
 * The two that arrive on their own.  Everywhere else a client says
 * something, it is answering a question somebody is holding right now.
 */
static void
test_the_two_loud_kinds(void)
{
    g_assert_cmpint(tier_of("message.refused", "reason", "a limit"), ==,
                    CLAWT_ALERT_ERROR);
    g_assert_cmpint(tier_of("image.finished", "error", "404"), ==,
                    CLAWT_ALERT_ERROR);
}

/*
 * A download that *succeeded* is routine.  Only the failure arrived on
 * its own with nobody watching, so classifying on the kind alone would
 * put every completed download in the loud list.
 */
static void
test_a_successful_download_is_routine(void)
{
    g_assert_cmpint(tier_of("image.finished", NULL, NULL), ==,
                    CLAWT_ALERT_ROUTINE);
}

/*
 * An agent that stopped when nobody asked it to is the one routine event
 * that is not routine -- and a notice rather than an error, because it
 * may well have been asked to.
 */
static void
test_a_bad_agent_state_is_a_notice(void)
{
    g_assert_cmpint(tier_of("agent.state", "state", "error"), ==,
                    CLAWT_ALERT_NOTICE);
    g_assert_cmpint(tier_of("agent.state", "state", "degraded"), ==,
                    CLAWT_ALERT_NOTICE);
    g_assert_cmpint(tier_of("agent.state", "state", "running"), ==,
                    CLAWT_ALERT_ROUTINE);
}

/*
 * One entry per percent would fill the whole list with one file, and
 * typing is a spinner rather than something that happened.
 */
static void
test_the_noisy_kinds_are_skipped(void)
{
    g_assert_cmpint(tier_of("image.progress", "total", "100"), ==,
                    CLAWT_ALERT_SKIP);
    g_assert_cmpint(tier_of("agent.typing", "typing", "true"), ==,
                    CLAWT_ALERT_SKIP);
}

/* Anything the daemon grows later is routine rather than dropped. */
static void
test_an_unknown_kind_is_routine(void)
{
    g_assert_cmpint(tier_of("something.new", NULL, NULL), ==,
                    CLAWT_ALERT_ROUTINE);
}

/* ── An alert arriving in front of the reader ──────────────────────── */

/*
 * The positive control, and it comes first deliberately: without it
 * these are one-sided assertions that would pass in a build whose badge
 * never counts anything at all.
 */
static void
test_an_alert_arriving_unseen_is_unread(void)
{
    g_assert_false(clawt_alert_arrives_read(FALSE, CLAWT_ALERT_ERROR));
    g_assert_false(clawt_alert_arrives_read(FALSE, CLAWT_ALERT_NOTICE));
}

static void
test_an_alert_arriving_in_front_of_you_is_read(void)
{
    g_assert_true(clawt_alert_arrives_read(TRUE, CLAWT_ALERT_ERROR));
    g_assert_true(clawt_alert_arrives_read(TRUE, CLAWT_ALERT_NOTICE));
}

/*
 * The routine stream is never counted, so it arrives read whatever is
 * on screen -- a flag no badge reads is one a later widening of the
 * filter would silently start believing.
 */
static void
test_a_routine_alert_is_read_either_way(void)
{
    g_assert_true(clawt_alert_arrives_read(FALSE, CLAWT_ALERT_ROUTINE));
    g_assert_true(clawt_alert_arrives_read(TRUE, CLAWT_ALERT_ROUTINE));
}

/* ── The chat column, which the composer has to agree with ─────────── */

/*
 * Both clients had the same fault: the transcript spends a gutter on
 * the avatar and the composer spent nothing, so the entry's frame stood
 * inside the column deliberately kept empty.  The numbers differ per
 * client, the derivation does not.
 */
static void
test_a_body_starts_past_the_gutter(void)
{
    /* The GTK client's pair. */
    g_assert_cmpint(clawt_chat_body_inset(12, 44), ==, 56);

    /* The web sheet's: an avatar and its gap, with no row inset. */
    g_assert_cmpint(clawt_chat_body_inset(0, 36), ==, 36);
}

/*
 * A client with no avatar column puts its composer against the clamp,
 * which is correct rather than a special case -- there is no gutter for
 * it to stand in.
 */
static void
test_no_gutter_means_no_inset(void)
{
    g_assert_cmpint(clawt_chat_body_inset(0, 0), ==, 0);
}

/* ── Where the alerts panel may push rather than overlay ───────────── */

/*
 * The derivation reproduces the number the client was built around.
 *
 * Asserted on the floor rather than on the breakpoint, because the
 * floor is the thing with a reason: (T - 600) / 2 + 12 >= 24 gives
 * T >= 624, and 280 + 624 / 0.74 is 1123.24 -- so 1124 is the first
 * width at which the column has its clear space.
 */
static void
test_the_push_threshold_is_derived(void)
{
    g_assert_cmpint(clawt_alerts_push_min_width(CLAWT_ALERTS_SIDEBAR_WIDTH,
                                                CLAWT_ALERTS_PANEL_FRACTION,
                                                CLAWT_CHAT_CLAMP_WIDTH,
                                                12),
                    ==, 1124);
}

/*
 * And the value the client ships clears it.
 *
 * This is the assertion that earns the extraction: the floor is built
 * from four numbers other lines of code own, and widening the panel or
 * the agent list moves it silently.  Nothing warns, because nothing is
 * broken -- the panel still opens and the transcript still renders,
 * with the column against the panel's edge.
 */
static void
test_the_shipped_breakpoint_clears_the_threshold(void)
{
    gint floor_width = clawt_alerts_push_min_width(
        CLAWT_ALERTS_SIDEBAR_WIDTH, CLAWT_ALERTS_PANEL_FRACTION,
        CLAWT_CHAT_CLAMP_WIDTH, 12);

    g_assert_cmpint(CLAWT_ALERTS_PUSH_BREAKPOINT, >=, floor_width);
}

/*
 * A wider panel needs a wider window, and a wider agent list moves the
 * whole thing along by its own width.  Two directions, because a
 * derivation that only ever gets its shipped inputs is a constant with
 * extra steps.
 */
static void
test_a_wider_panel_needs_a_wider_window(void)
{
    gint narrow = clawt_alerts_push_min_width(280, 0.26, 600, 12);
    gint wide = clawt_alerts_push_min_width(280, 0.40, 600, 12);

    g_assert_cmpint(wide, >, narrow);
    g_assert_cmpint(clawt_alerts_push_min_width(400, 0.26, 600, 12), ==,
                    narrow + 120);
}

/*
 * A row margin at or above the wanted gap means the column needs
 * nothing beyond its clamp -- the margin is already the clear space.
 * It must never ask for *less* than the clamp, which an unclamped
 * subtraction would do the moment somebody raised the margin.
 */
static void
test_a_generous_row_margin_never_shrinks_the_column(void)
{
    g_assert_cmpint(clawt_alerts_push_min_width(280, 0.26, 600, 24), ==,
                    clawt_alerts_push_min_width(280, 0.26, 600, 999));
    g_assert_cmpint(clawt_alerts_push_min_width(280, 0.26, 600, 999), ==,
                    280 + (gint)ceil(600.0 / 0.74));
}

/*
 * A fraction that leaves the transcript nothing describes no layout, so
 * it refuses rather than returning a number that looks usable.
 */
static void
test_an_impossible_fraction_is_refused(void)
{
    g_assert_cmpint(clawt_alerts_push_min_width(280, 0.0, 600, 12), ==, 0);
    g_assert_cmpint(clawt_alerts_push_min_width(280, 1.0, 600, 12), ==, 0);
    g_assert_cmpint(clawt_alerts_push_min_width(280, 1.5, 600, 12), ==, 0);
}

/*
 * A fleet reply's agent array, built by hand.
 *
 * A teamless agent gets `team: null`, because that is what the daemon
 * actually sends -- checked against a running one rather than assumed.
 * Three spellings can reach a client for "no team": the member absent,
 * the member null, and the member "". The first two arrive from the
 * daemon and the third from an agent taken off a team, so all three are
 * covered below; a fixture that produced only one would leave the rule
 * asserted for the case that does not happen.
 */
static JsonArray *
fleet(const gchar *spec)
{
    JsonArray *agents = json_array_new();
    g_auto(GStrv) rows = g_strsplit(spec, ";", -1);
    gsize i;

    for (i = 0; rows[i] != NULL && *rows[i] != '\0'; i++) {
        g_auto(GStrv) parts = g_strsplit(rows[i], ",", 3);
        JsonObject *agent = json_object_new();

        json_object_set_string_member(agent, "id", parts[0]);

        if (parts[1] != NULL && *parts[1] != '\0')
            json_object_set_string_member(agent, "team", parts[1]);
        else
            json_object_set_null_member(agent, "team");

        json_object_set_string_member(agent, "state",
                                      (parts[2] != NULL &&
                                       strchr(parts[2], 'r') != NULL)
                                      ? "running" : "stopped");
        json_object_set_boolean_member(agent, "busy",
                                       parts[2] != NULL &&
                                       strchr(parts[2], 'b') != NULL);

        json_array_add_object_element(agents, agent);
    }

    return agents;
}

/*
 * A team heading counts what is working, not only what is up.
 *
 * Agents are started once and stay running, so running/total barely
 * moves; whether anybody is *doing* something is the part that changes
 * minute to minute, and it was the part the heading threw away -- at
 * exactly the level where the rows that draw it are folded out of sight.
 */
static void
test_the_tally_counts_what_is_working(void)
{
    g_autoptr(JsonArray) agents =
        fleet("a,build,rb;b,build,r;c,build,;d,ship,rb;e,,rb");
    guint total = 0, running = 0, busy = 0;

    clawt_team_tally(agents, "build", &total, &running, &busy);

    g_assert_cmpuint(total, ==, 3);
    g_assert_cmpuint(running, ==, 2);
    g_assert_cmpuint(busy, ==, 1);

    /* A different team is counted separately, not cumulatively. */
    clawt_team_tally(agents, "ship", &total, &running, &busy);

    g_assert_cmpuint(total, ==, 1);
    g_assert_cmpuint(running, ==, 1);
    g_assert_cmpuint(busy, ==, 1);
}

/*
 * A team where nobody is working reports zero.
 *
 * The positive control for the assertion above: "busy is 1" would pass
 * in a build that answered 1 for everything, and a spinner that is
 * always on is worse than no spinner.
 */
static void
test_an_idle_team_reports_nobody_working(void)
{
    g_autoptr(JsonArray) agents = fleet("a,build,r;b,build,r;c,build,");
    guint total = 0, running = 0, busy = 0;

    clawt_team_tally(agents, "build", &total, &running, &busy);

    g_assert_cmpuint(total, ==, 3);
    g_assert_cmpuint(running, ==, 2);
    g_assert_cmpuint(busy, ==, 0);
}

/*
 * The teamless group is a group.
 *
 * It is where the chief of staff lives, so "is anything happening" is at
 * least as relevant there.  NULL and "" name the same group: the GTK
 * client asked with NULL and the web client with "", so the two would
 * have disagreed the first time they met a spelling the other did not
 * default to.
 *
 * And all three spellings of "no team" have to answer alike, which is
 * the half a fixture can get wrong without anybody noticing: the daemon
 * sends `team: null`, an older reply may omit the member, and an agent
 * taken off a team has "".  The sidebar has already been wrong once
 * because a sentinel and a real value were both spelled as absence.
 */
static void
test_the_teamless_group_is_counted_either_way(void)
{
    g_autoptr(JsonArray) agents = fleet("chief,,rb;a,build,r;b,,r");
    guint total = 0, running = 0, busy = 0;

    clawt_team_tally(agents, NULL, &total, &running, &busy);

    g_assert_cmpuint(total, ==, 2);
    g_assert_cmpuint(running, ==, 2);
    g_assert_cmpuint(busy, ==, 1);

    clawt_team_tally(agents, "", &total, &running, &busy);

    g_assert_cmpuint(total, ==, 2);
    g_assert_cmpuint(running, ==, 2);
    g_assert_cmpuint(busy, ==, 1);

    /*
     * The three spellings of "no team" in one array, counted as one
     * group.  fleet() writes null; these two are added by hand because
     * they are the shapes it cannot produce.
     */
    {
        g_autoptr(JsonArray) mixed = json_array_new();
        JsonObject *absent = json_object_new();
        JsonObject *empty = json_object_new();
        JsonObject *null_team = json_object_new();

        json_object_set_string_member(absent, "id", "absent");
        json_object_set_string_member(absent, "state", "running");
        json_object_set_boolean_member(absent, "busy", TRUE);

        json_object_set_string_member(empty, "id", "empty");
        json_object_set_string_member(empty, "team", "");
        json_object_set_string_member(empty, "state", "running");
        json_object_set_boolean_member(empty, "busy", FALSE);

        json_object_set_string_member(null_team, "id", "null");
        json_object_set_null_member(null_team, "team");
        json_object_set_string_member(null_team, "state", "stopped");
        json_object_set_boolean_member(null_team, "busy", FALSE);

        json_array_add_object_element(mixed, absent);
        json_array_add_object_element(mixed, empty);
        json_array_add_object_element(mixed, null_team);

        clawt_team_tally(mixed, NULL, &total, &running, &busy);

        g_assert_cmpuint(total, ==, 3);
        g_assert_cmpuint(running, ==, 2);
        g_assert_cmpuint(busy, ==, 1);
    }
}

/*
 * A busy agent that is not running is not counted as working.
 *
 * Nothing should produce that, and if something does the heading must
 * not claim a stopped agent is mid-turn.
 */
static void
test_only_a_running_agent_can_be_working(void)
{
    g_autoptr(JsonArray) agents = fleet("a,build,b;b,build,rb");
    guint total = 0, running = 0, busy = 0;

    clawt_team_tally(agents, "build", &total, &running, &busy);

    g_assert_cmpuint(total, ==, 2);
    g_assert_cmpuint(running, ==, 1);
    g_assert_cmpuint(busy, ==, 1);
}


/*
 * Which of an agent's rooms is a conversation, and with whom.
 *
 * The operator's chat and a peer conversation are the same shape -- two
 * members, one of them this agent -- and the difference is only who the
 * other one is. Answered here so neither client takes a room id apart,
 * which is already recorded as the reason the daemon reports `dm_room`
 * rather than letting a client build it.
 */
static void
test_a_conversation_names_its_other_party(void)
{
    const gchar *with_user[] = { "user", "oryx", NULL };
    const gchar *with_peer[] = { "oryx", "gnuisaince", NULL };
    const gchar *standup[] = { "oryx", "kudu", "mamba", NULL };
    const gchar *elsewhere[] = { "kudu", "mamba", NULL };
    const gchar *alone[] = { "oryx", NULL };
    const gchar *doubled[] = { "oryx", "oryx", NULL };

    /* The operator's own chat, which is the one the client opens on. */
    g_assert_cmpstr(clawt_chat_conversation_peer(with_user, "oryx"), ==,
                    "user");

    /* And a peer's, whichever order the members happen to be in. */
    g_assert_cmpstr(clawt_chat_conversation_peer(with_peer, "oryx"), ==,
                    "gnuisaince");
    g_assert_cmpstr(clawt_chat_conversation_peer(with_peer, "gnuisaince"),
                    ==, "oryx");

    /* A room of three is not a conversation between two. */
    g_assert_null(clawt_chat_conversation_peer(standup, "oryx"));

    /* Nor is one this agent is not in. */
    g_assert_null(clawt_chat_conversation_peer(elsewhere, "oryx"));

    /* Nor a room of one, which is what a half-built config produces. */
    g_assert_null(clawt_chat_conversation_peer(alone, "oryx"));

    /*
     * And an agent listed twice is a room of two that is not a
     * conversation with anybody. Returning "oryx" here would put an
     * agent in a chat with itself in the switcher.
     */
    g_assert_null(clawt_chat_conversation_peer(doubled, "oryx"));

    g_assert_null(clawt_chat_conversation_peer(NULL, "oryx"));
    g_assert_null(clawt_chat_conversation_peer(with_peer, NULL));
}


/*
 * The "4m ago" rule, which both clients and the orchestration tools
 * draw.  It was two byte-identical copies in two clients and about to
 * become a third; the comments above them had already drifted, one
 * claiming "4 minutes ago" for code that writes "4m ago".
 *
 * `now` is a parameter, which is the whole reason these boundaries can
 * be asserted at all -- a helper calling g_get_real_time() itself can
 * only be tested at whatever time it happens to be.
 */
static void
test_relative_ages_read_the_way_they_are_written(void)
{
    gint64 now = 1000000000LL * G_USEC_PER_SEC;
    struct { gint64 seconds_ago; const gchar *want; } cases[] = {
        {     0, "just now" },
        {    59, "just now" },   /* the last second before minutes */
        {    60, "1m ago" },
        {  3599, "59m ago" },    /* and the last before hours */
        {  3600, "1h ago" },
        { 86399, "23h ago" },
        { 86400, "1d ago" },
        { 172800, "2d ago" }
    };
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_autofree gchar *got = clawt_time_ago_label(
            now - cases[i].seconds_ago * G_USEC_PER_SEC, now);

        g_assert_cmpstr(got, ==, cases[i].want);
    }
}

/*
 * A clock that went backwards must not produce "-3m ago".  A task list
 * is not the place to report that the machine's time moved.
 */
static void
test_a_future_timestamp_reads_as_just_now(void)
{
    gint64 now = 1000000000LL * G_USEC_PER_SEC;
    g_autofree gchar *got = clawt_time_ago_label(now + 3600LL * G_USEC_PER_SEC,
                                                 now);

    g_assert_cmpstr(got, ==, "just now");
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/client-rules/team-tally/counts-working",
                    test_the_tally_counts_what_is_working);
    g_test_add_func("/client-rules/team-tally/idle-team",
                    test_an_idle_team_reports_nobody_working);
    g_test_add_func("/client-rules/team-tally/teamless",
                    test_the_teamless_group_is_counted_either_way);
    g_test_add_func("/client-rules/team-tally/busy-implies-running",
                    test_only_a_running_agent_can_be_working);
    g_test_add_func("/client-rules/unread/elsewhere",
                    test_a_message_elsewhere_counts);
    g_test_add_func("/client-rules/unread/on-screen",
                    test_the_room_on_screen_does_not);
    g_test_add_func("/client-rules/unread/no-room-on-screen",
                    test_no_room_on_screen_still_counts);
    g_test_add_func("/client-rules/unread/your-own",
                    test_your_own_message_does_not);
    g_test_add_func("/client-rules/unread/replayed",
                    test_a_replayed_message_does_not);
    g_test_add_func("/client-rules/unread/undated",
                    test_an_undated_message_counts);
    g_test_add_func("/client-rules/unread/incomplete",
                    test_nothing_without_a_room_or_a_sender);

    g_test_add_func("/client-rules/follow/at-the-bottom",
                    test_at_the_bottom_follows);
    g_test_add_func("/client-rules/follow/away",
                    test_away_from_the_bottom_does_not);
    g_test_add_func("/client-rules/follow/boundary",
                    test_the_tolerance_boundary);
    g_test_add_func("/client-rules/follow/short-transcript",
                    test_a_short_transcript_is_always_at_the_bottom);

    g_test_add_func("/client-rules/tier/loud", test_the_two_loud_kinds);
    g_test_add_func("/client-rules/tier/download-ok",
                    test_a_successful_download_is_routine);
    g_test_add_func("/client-rules/tier/bad-state",
                    test_a_bad_agent_state_is_a_notice);
    g_test_add_func("/client-rules/tier/skipped",
                    test_the_noisy_kinds_are_skipped);
    g_test_add_func("/client-rules/tier/unknown",
                    test_an_unknown_kind_is_routine);

    g_test_add_func("/client-rules/alert-arrival/unseen",
                    test_an_alert_arriving_unseen_is_unread);
    g_test_add_func("/client-rules/alert-arrival/in-front-of-you",
                    test_an_alert_arriving_in_front_of_you_is_read);
    g_test_add_func("/client-rules/alert-arrival/routine",
                    test_a_routine_alert_is_read_either_way);

    g_test_add_func("/client-rules/column/body-starts-past-the-gutter",
                    test_a_body_starts_past_the_gutter);
    g_test_add_func("/client-rules/column/no-gutter",
                    test_no_gutter_means_no_inset);

    g_test_add_func("/client-rules/alerts-push/derived",
                    test_the_push_threshold_is_derived);
    g_test_add_func("/client-rules/alerts-push/shipped-clears-it",
                    test_the_shipped_breakpoint_clears_the_threshold);
    g_test_add_func("/client-rules/alerts-push/wider-panel",
                    test_a_wider_panel_needs_a_wider_window);
    g_test_add_func("/client-rules/alerts-push/generous-margin",
                    test_a_generous_row_margin_never_shrinks_the_column);
    g_test_add_func("/client-rules/conversation/names-its-other-party",
                    test_a_conversation_names_its_other_party);
    g_test_add_func("/client-rules/alerts-push/impossible-fraction",
                    test_an_impossible_fraction_is_refused);

    g_test_add_func("/client-rules/age/boundaries",
                    test_relative_ages_read_the_way_they_are_written);
    g_test_add_func("/client-rules/age/backwards-clock",
                    test_a_future_timestamp_reads_as_just_now);

    return g_test_run();
}
