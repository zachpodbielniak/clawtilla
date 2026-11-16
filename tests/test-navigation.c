/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "clawtilla.h"

/* User-facing examples protect matching behavior, including negative
 * controls: an implementation which returns TRUE for everything fails. */
static void
test_navigation_matching(void)
{
	g_assert_true(clawt_navigation_matches(NULL, NULL));
	g_assert_true(clawt_navigation_matches("  \t", "Agent Alice"));
	g_assert_true(clawt_navigation_matches("ALIC", "Alice Agent alice research"));
	g_assert_true(clawt_navigation_matches("research ali", "Alice Agent alice research"));
	g_assert_true(clawt_navigation_matches("elodie", "Élodie Agent writer"));
	g_assert_true(clawt_navigation_matches("réu", "Réunion Room standup"));
	g_assert_true(clawt_navigation_matches("work dec", "Decisions Work decisions"));
	g_assert_false(clawt_navigation_matches("work mail", "Decisions Work decisions"));
	g_assert_false(clawt_navigation_matches("alice", "Bob Agent bob"));
	g_assert_false(clawt_navigation_matches("alice", NULL));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/navigation/matching", test_navigation_matching);
	return g_test_run();
}
