/*
 * test-log-level.c - What daemon.log_level turns off, and what it must not
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * daemon.log_level was accepted, saved, echoed back and read by nothing
 * at all until 0.2.0 -- a setting whose documentation described logging
 * that did not exist, on a daemon that ignored the key.
 *
 * The rule it now carries is easy to get right and impossible to notice
 * wrong: %GLogLevelFlags is a bitmask whose *lower* numeric values are
 * the more severe ones, so "at or above the ceiling" reads as a `<=`.
 * Inverting it swaps quiet and verbose, which produces a daemon that
 * looks configured and says the opposite of what it was asked to.
 *
 * The levels are walked out of the enum the schema already names, never
 * spelled out here: a hand-written copy of an option's values is the one
 * thing this codebase has watched drift every time.
 */

#include <clawtilla.h>

/*
 * The four levels, in the order the enum declares them, most severe
 * first -- which is also the order their GLib flags sort in.
 */
static const GLogLevelFlags glib_levels[] = {
    G_LOG_LEVEL_ERROR,
    G_LOG_LEVEL_WARNING,
    G_LOG_LEVEL_INFO,
    G_LOG_LEVEL_DEBUG
};

/*
 * Every level permits itself and everything more severe, and nothing
 * less severe. Written as a walk over the whole matrix rather than as a
 * handful of examples, so a ceiling that permits one level too many is
 * caught wherever it sits rather than only at the boundary somebody
 * happened to write a case for.
 */
static void
test_a_ceiling_permits_itself_and_everything_above(void)
{
    gsize ceiling;
    gsize level;

    for (ceiling = 0; ceiling < G_N_ELEMENTS(glib_levels); ceiling++) {
        for (level = 0; level < G_N_ELEMENTS(glib_levels); level++) {
            gboolean permitted =
                clawt_log_level_permits((ClawtLogLevel)ceiling,
                                        glib_levels[level]);

            /*
             * A lower index is a more severe level, so a message is
             * written exactly when its index is at or below the
             * ceiling's.
             */
            g_assert_cmpint(permitted, ==, level <= ceiling);
        }
    }
}

/*
 * The enum has exactly the four members this file walks.
 *
 * Asked of the type rather than assumed, because the matrix above is
 * only as complete as the array it iterates: a fifth level added to
 * ClawtLogLevel would otherwise be untested by a test that still passes.
 */
static void
test_the_walk_covers_every_level(void)
{
    g_autoptr(GEnumClass) klass = g_type_class_ref(CLAWT_TYPE_LOG_LEVEL);

    g_assert_cmpuint(klass->n_values, ==, G_N_ELEMENTS(glib_levels));
}

/*
 * The default is info, and info is not the quietest thing available --
 * so a daemon nobody has configured says more than one at `error`.
 *
 * Worth its own assertion because CLAWT_LOG_ERROR is 0: a value the enum
 * cannot resolve becoming 0 would quieten the daemon to almost nothing
 * while looking like a working configuration.
 */
static void
test_the_default_is_not_the_quietest(void)
{
    g_assert_true(clawt_log_level_permits(CLAWT_LOG_INFO,
                                          G_LOG_LEVEL_MESSAGE));
    g_assert_false(clawt_log_level_permits(CLAWT_LOG_ERROR,
                                           G_LOG_LEVEL_MESSAGE));
}

/*
 * A critical is not a level anybody configures, but it is one GLib
 * emits, and it must not fall through a gap between warning and error.
 */
static void
test_a_critical_is_ranked_with_the_failures(void)
{
    g_assert_true(clawt_log_level_permits(CLAWT_LOG_WARNING,
                                          G_LOG_LEVEL_CRITICAL));
    g_assert_true(clawt_log_level_permits(CLAWT_LOG_DEBUG,
                                          G_LOG_LEVEL_CRITICAL));

    /*
     * And `error` is the level whose whole point is "only failures", so
     * a critical is one of the things it keeps.
     */
    g_assert_true(clawt_log_level_permits(CLAWT_LOG_ERROR,
                                          G_LOG_LEVEL_CRITICAL));
}

/*
 * Anything outside the six known levels is written rather than dropped.
 *
 * A custom level is a number the scale does not describe, and GLib emits
 * structured records of its own with no level bits set at all. Judging
 * either against the ceiling would silently delete it -- and a filter
 * that hides what it does not recognise is how a log stops being a
 * record. So the quietest possible ceiling still passes them through.
 */
static void
test_an_unknown_level_is_not_swallowed(void)
{
    GLogLevelFlags custom = 1 << 10;

    g_assert_true(clawt_log_level_permits(CLAWT_LOG_ERROR, custom));
    g_assert_true(clawt_log_level_permits(CLAWT_LOG_ERROR, 0));
}

/*
 * The flag bits GLib carries alongside the level -- FATAL and RECURSION
 * -- must not change the answer. They ride in the same word, so a
 * comparison that forgets to mask makes a fatal warning compare as a
 * much larger number and vanish at every ceiling.
 */
static void
test_the_flag_bits_do_not_change_the_answer(void)
{
    g_assert_true(clawt_log_level_permits(
        CLAWT_LOG_WARNING, G_LOG_LEVEL_WARNING | G_LOG_FLAG_FATAL));
    g_assert_false(clawt_log_level_permits(
        CLAWT_LOG_WARNING, G_LOG_LEVEL_DEBUG | G_LOG_FLAG_FATAL));
}

/*
 * Every nick the schema offers resolves, and each resolves to a
 * different level.
 *
 * This is the wire between the configuration file and the predicate
 * above: the daemon reads the key as a string and resolves it through
 * this enum, so a nick the type does not carry is a level nobody can
 * select however correct the comparison is.
 */
static void
test_every_documented_nick_resolves(void)
{
    g_autoptr(GEnumClass) klass = g_type_class_ref(CLAWT_TYPE_LOG_LEVEL);
    guint i;
    gint unknown = 99;

    for (i = 0; i < klass->n_values; i++) {
        const gchar *nick = klass->values[i].value_nick;
        gint value = -1;

        g_assert_nonnull(nick);
        g_assert_true(clawt_enum_from_nick(CLAWT_TYPE_LOG_LEVEL, nick,
                                           &value));
        g_assert_cmpint(value, ==, klass->values[i].value);

        /* And it round-trips, so the two directions cannot disagree. */
        g_assert_cmpstr(clawt_enum_to_nick(CLAWT_TYPE_LOG_LEVEL, value),
                        ==, nick);
    }

    /* And a nick the type does not have is refused, not folded to 0. */
    g_assert_false(clawt_enum_from_nick(CLAWT_TYPE_LOG_LEVEL, "verbose",
                                        &unknown));
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/log-level/ceiling-permits-itself-and-above",
                    test_a_ceiling_permits_itself_and_everything_above);
    g_test_add_func("/log-level/the-walk-covers-every-level",
                    test_the_walk_covers_every_level);
    g_test_add_func("/log-level/the-default-is-not-the-quietest",
                    test_the_default_is_not_the_quietest);
    g_test_add_func("/log-level/a-critical-ranks-with-the-failures",
                    test_a_critical_is_ranked_with_the_failures);
    g_test_add_func("/log-level/an-unknown-level-is-not-swallowed",
                    test_an_unknown_level_is_not_swallowed);
    g_test_add_func("/log-level/flag-bits-do-not-change-the-answer",
                    test_the_flag_bits_do_not_change_the_answer);
    g_test_add_func("/log-level/every-documented-nick-resolves",
                    test_every_documented_nick_resolves);

    return g_test_run();
}
