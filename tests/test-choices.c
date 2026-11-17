/*
 * test-choices.c - The fixed lists behind the settings' drop-downs
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A drop-down only helps if every entry it offers is one the daemon
 * takes.  Each test here holds one list (src/config/clawt-choices.c)
 * against the code that reads its values -- the cron preset parser, the
 * reviewer resolver, the screen-size parser, the schedule enumeration,
 * the schema's own sentence -- so a list and its reader cannot drift
 * apart without a red test.
 */

#include <clawtilla.h>

#include <string.h>

/*
 * Every weekday is one clawt_cron_from_preset() turns into a weekly
 * expression on that day.  Were one refused, choosing it in a routine's
 * form would save a routine that never runs.
 */
static void
test_weekdays_parse(void)
{
	guint i;

	g_assert_cmpuint(clawt_weekday_count(), ==, 7);

	for (i = 0; i < clawt_weekday_count(); i++) {
		g_autoptr(GError) error = NULL;
		g_autofree gchar *expression = NULL;
		g_autofree gchar *expected = NULL;

		expression = clawt_cron_from_preset("weekly", "09:00",
		                                    clawt_weekday_nth(i), NULL,
		                                    &error);
		g_assert_no_error(error);

		/* Sunday is 0, as cron counts and the list is ordered */
		expected = g_strdup_printf("0 9 * * %u", i);
		g_assert_cmpstr(expression, ==, expected);
		g_assert_nonnull(clawt_weekday_nth_label(i));
	}

	g_assert_null(clawt_weekday_nth(clawt_weekday_count()));
}

/*
 * The schedules are the enumeration's nicks, in its order, each with a
 * written label (not the capitalised-nick fallback), and each a preset
 * the parser knows.
 */
static void
test_schedules_are_the_enumeration(void)
{
	GEnumClass *klass = g_type_class_ref(CLAWT_TYPE_SCHEDULE);
	guint i;

	g_assert_cmpuint(clawt_schedule_count(), ==, klass->n_values);

	for (i = 0; i < clawt_schedule_count(); i++) {
		const gchar *nick = clawt_schedule_nth(i);
		g_autoptr(GError) error = NULL;
		g_autofree gchar *expression = NULL;

		g_assert_cmpstr(nick, ==, klass->values[i].value_nick);
		g_assert_cmpstr(clawt_schedule_nth_label(i), !=, nick);

		expression = clawt_cron_from_preset(nick, "09:00", "monday",
		                                    "0 9 * * *", &error);
		g_assert_no_error(error);
		/* manual alone has no next time */
		g_assert_true((expression == NULL) ==
		              (g_strcmp0(nick, "manual") == 0));
	}

	g_assert_null(clawt_schedule_nth(clawt_schedule_count()));
	g_type_class_unref(klass);
}

/*
 * Every fixed reviewer is a spelling clawt_task_resolve_reviewer()
 * accepts, resolving to what its label promises.  The "" entry is the
 * one a project shows as "each agent's own" and an agent as "no
 * review", so the two labels must differ.
 */
static void
test_reviewers_resolve(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(ClawtConfig) config = clawt_config_load_from_string(
		"teams:\n  - id: build\n"
		"agents:\n"
		"  - id: chief\n    chief_of_staff: true\n"
		"  - id: lead\n    team: build\n    team_role: lead\n"
		"  - id: hand\n    team: build\n", &error);
	static const gchar *const expected[] = {
		NULL, NULL, "user", "chief", "lead"
	};
	g_autofree gchar *agent_label = NULL;
	guint i;

	g_assert_no_error(error);
	g_assert_cmpuint(clawt_review_by_count(), ==, G_N_ELEMENTS(expected));

	for (i = 0; i < clawt_review_by_count(); i++) {
		g_autoptr(GError) resolve_error = NULL;
		g_autofree gchar *who = clawt_task_resolve_reviewer(
			config, clawt_review_by_nth(i), "chief", "hand",
			&resolve_error);

		g_assert_no_error(resolve_error);
		g_assert_cmpstr(who, ==, expected[i]);
		g_assert_nonnull(clawt_review_by_nth_label(i, TRUE));
		g_assert_nonnull(clawt_review_by_nth_label(i, FALSE));
	}

	g_assert_cmpstr(clawt_review_by_nth(0), ==, "");
	g_assert_cmpstr(clawt_review_by_nth_label(0, TRUE), !=,
	                clawt_review_by_nth_label(0, FALSE));

	/* an agent's entry names it */
	agent_label = clawt_review_by_agent_label("lead");
	g_assert_nonnull(strstr(agent_label, "lead"));
	g_assert_null(clawt_review_by_nth(clawt_review_by_count()));
}

/*
 * Every size is one the screen-size parser reads, narrowest first, with
 * the display computer's default (1280x800) among them -- else a new
 * display agent's form would open on a size it does not have.
 */
static void
test_resolutions_parse(void)
{
	guint64 previous = 0;
	gboolean has_default = FALSE;
	guint i;

	for (i = 0; i < clawt_resolution_count(); i++) {
		const gchar *size = clawt_resolution_nth(i);
		guint width = 0;
		guint height = 0;

		g_assert_true(clawt_vm_computer_parse_resolution(size, &width,
		                                                 &height));
		g_assert_cmpuint(width, >=, previous);
		previous = width;
		has_default |= (g_strcmp0(size, "1280x800") == 0);
	}

	g_assert_true(has_default);
	g_assert_null(clawt_resolution_nth(clawt_resolution_count()));
}

/*
 * The effort levels are the ones model.effort's schema sentence names,
 * after "" for the model's default: the clients' list and the
 * documented one are the same five.
 */
static void
test_efforts_match_the_schema(void)
{
	const ClawtSchemaEntry *entry =
		clawt_config_schema_lookup("agents.model.effort");
	guint i;

	if (entry == NULL)
		entry = clawt_config_schema_lookup("model.effort");
	g_assert_nonnull(entry);

	g_assert_cmpuint(clawt_effort_count(), ==, 6);
	g_assert_cmpstr(clawt_effort_nth(0), ==, "");

	for (i = 1; i < clawt_effort_count(); i++) {
		g_assert_nonnull(strstr(entry->doc, clawt_effort_nth(i)));
		g_assert_nonnull(clawt_effort_nth_label(i));
	}

	g_assert_null(clawt_effort_nth(clawt_effort_count()));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/choices/weekdays-parse", test_weekdays_parse);
	g_test_add_func("/choices/schedules-are-the-enumeration",
	                test_schedules_are_the_enumeration);
	g_test_add_func("/choices/reviewers-resolve", test_reviewers_resolve);
	g_test_add_func("/choices/resolutions-parse", test_resolutions_parse);
	g_test_add_func("/choices/efforts-match-the-schema",
	                test_efforts_match_the_schema);

	return g_test_run();
}
