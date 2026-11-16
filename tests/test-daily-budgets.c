/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <clawtilla.h>
#include <glib/gstdio.h>
#include <sys/socket.h>
#include <unistd.h>
#include "clawt-test-util.h"

/* Persist real usage rows so reconstruction tests the production reader. */
static void
add_cost(ClawtConfig *config, const gchar *agent, gint64 cost, gint64 time)
{
	g_autofree gchar *state = clawt_config_agent_state_dir(config, agent);
	g_autofree gchar *sessions = g_build_filename(state, "sessions", NULL);
	g_autofree gchar *path = clawt_usage_database_path(state);
	g_autoptr(LcDatabase) db = LC_DATABASE(lc_sqlite_database_new());
	g_autoptr(GError) error = NULL;

	g_assert_cmpint(g_mkdir_with_parents(sessions, 0700), ==, 0);
	g_assert_true(lc_database_open(db, path, &error));
	g_assert_true(lc_database_add_token_usage(db, "s", "clawtilla", "r",
		"test", "test", 1, 1, cost, time, &error));
	g_assert_no_error(error);
	lc_database_close(db);
}

/* Covers exact integer thresholds, independent recipients, persistence and days. */
static void
test_daily(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("clawt-daily-XXXXXX", NULL);
	g_autofree gchar *yaml = g_strdup_printf(
		"daemon:\n  state_dir: '%s'\ndefaults:\n  workspace_root: '%s/work'\n"
		"orchestration:\n  daily_fleet_budget_micros: 100\n"
		"  daily_agent_budget_micros: 80\nagents:\n"
		"  - id: alice\n    daily_agent_budget_micros: 40\n  - id: bob\n", dir, dir);
	g_autoptr(GError) error = NULL;
	g_autoptr(ClawtConfig) config = clawt_config_load_from_string(yaml, &error);
	g_autoptr(ClawtAgentManager) agents = NULL;
	g_autoptr(ClawtRoomManager) rooms = clawt_room_manager_new(NULL);
	g_autoptr(ClawtMailboxRouter) router = NULL;
	g_autoptr(ClawtEventBus) bus = clawt_event_bus_new(20);
	g_autoptr(GDateTime) today = g_date_time_new_now_local();
	g_autoptr(GDateTime) tomorrow = g_date_time_add_days(today, 1);
	gint64 now = g_date_time_to_unix(today);
	guint i;

	g_assert_no_error(error);
	agents = clawt_agent_manager_new(config);
	g_assert_cmpint(clawt_agent_config_get_int(clawt_config_get_agent(config, "bob"),
		"daily_agent_budget_micros"), ==, 80);
	router = clawt_mailbox_router_new(agents, rooms, NULL);
	clawt_mailbox_router_set_event_bus(router, bus);
	/* Pending/concurrent admissions make no speculative reservation. */
	for (i = 0; i < 10; i++)
		g_assert_true(clawt_mailbox_router_check_daily_budget(router, "alice", now, &error));
	add_cost(config, "alice", 39, now);
	g_assert_true(clawt_mailbox_router_check_daily_budget(router, "alice", now, &error));
	add_cost(config, "alice", 1, now);
	g_assert_false(clawt_mailbox_router_check_daily_budget(router, "alice", now, &error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT);
	g_clear_error(&error);
	g_assert_cmpuint(clawt_event_bus_get_cursor(bus), ==, 1);
	for (i = 0; i < 3; i++) {
		g_assert_false(clawt_mailbox_router_check_daily_budget(router, "alice", now, &error));
		g_clear_error(&error);
	}
	g_assert_cmpuint(clawt_event_bus_get_cursor(bus), ==, 1);
	/* Reset sessions without discarding today's spend. */
	{
		g_autofree gchar *state = clawt_config_agent_state_dir(config, "alice");
		g_autofree gchar *old = g_build_filename(state, "sessions", NULL);
		g_autofree gchar *archive = g_build_filename(state, "sessions.reset-test", NULL);
		g_assert_cmpint(g_rename(old, archive), ==, 0);
	}
	g_assert_true(clawt_mailbox_router_check_daily_budget(router, "bob", now, &error));
	/* Reconstruct the router, as after daemon restart: the cap remains reached. */
	g_clear_object(&router);
	router = clawt_mailbox_router_new(agents, rooms, NULL);
	clawt_mailbox_router_set_event_bus(router, bus);
	g_assert_false(clawt_mailbox_router_check_daily_budget(router, "alice", now, &error));
	g_clear_error(&error);
	add_cost(config, "bob", 60, now);
	g_assert_false(clawt_mailbox_router_check_daily_budget(router, "bob", now, &error));
	g_clear_error(&error);
	g_assert_true(clawt_mailbox_router_check_daily_budget(router, "alice",
		g_date_time_to_unix(tomorrow), &error));
	g_assert_no_error(error);
	add_cost(config, "alice", 40, g_date_time_to_unix(tomorrow));
	g_assert_false(clawt_mailbox_router_check_daily_budget(router, "alice",
		g_date_time_to_unix(tomorrow), &error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT);
	g_clear_error(&error);
	g_assert_cmpuint(clawt_event_bus_get_cursor(bus), ==, 4);
	/* An unreadable existing ledger must refuse even without an error sink. */
	{
		g_autofree gchar *state = clawt_config_agent_state_dir(config, "bob");
		g_autofree gchar *path = clawt_usage_database_path(state);
		g_assert_cmpint(g_unlink(path), ==, 0);
		g_assert_cmpint(g_mkdir(path, 0700), ==, 0);
		g_assert_false(clawt_mailbox_router_check_daily_budget(router, "bob", now, NULL));
	}
	g_clear_object(&router);
	g_clear_object(&agents);
	g_clear_object(&rooms);
	g_clear_object(&config);
	clawt_test_remove_tree(dir);
}

/* Prove the guard is wired before queueing and before leasing durable work. */
static void
test_delivery(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("clawt-daily-delivery-XXXXXX", NULL);
	g_autofree gchar *yaml = g_strdup_printf(
		"daemon:\n  state_dir: '%s'\ndefaults:\n  workspace_root: '%s/work'\n"
		"orchestration:\n  daily_fleet_budget_micros: 1\nagents:\n  - id: alice\n",
		dir, dir);
	g_autoptr(GError) error = NULL;
	g_autoptr(ClawtConfig) config = clawt_config_load_from_string(yaml, &error);
	g_autoptr(ClawtAgentManager) agents = clawt_agent_manager_new(config);
	g_autoptr(ClawtRoomManager) rooms = clawt_room_manager_new(NULL);
	g_autoptr(ClawtMailboxRouter) router = clawt_mailbox_router_new(agents, rooms, NULL);
	g_autoptr(GSocket) near_end = NULL;
	g_autoptr(GSocket) far_end = NULL;
	g_autoptr(GSocketConnection) connection = NULL;
	g_autoptr(ClawtLink) link = NULL;
	ClawtAgent *agent;
	ClawtMailbox *mailbox;
	int fds[2];

	g_assert_no_error(error);
	g_assert_true(clawt_agent_manager_load(agents, &error));
	agent = clawt_agent_manager_get(agents, "alice");
	mailbox = clawt_agent_get_mailbox(agent);
	g_assert_cmpint(clawt_mailbox_router_send_to(router, "user", "alice",
		"queued before cost", NULL, 0, &error), ==, 1);
	add_cost(config, "alice", 1, g_get_real_time() / G_USEC_PER_SEC);
	g_assert_cmpint(clawt_mailbox_router_send_to(router, "user", "alice",
		"refused after cost", NULL, 0, &error), ==, -1);
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT);
	g_clear_error(&error);
	g_assert_cmpuint(clawt_mailbox_depth(mailbox), ==, 1);
	g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), ==, 0);
	near_end = g_socket_new_from_fd(fds[0], &error);
	far_end = g_socket_new_from_fd(fds[1], &error);
	connection = g_socket_connection_factory_create_connection(near_end);
	link = clawt_link_new(connection);
	clawt_agent_set_link(agent, link);
	g_assert_true(clawt_link_is_open(link));
	g_assert_cmpuint(clawt_mailbox_router_drain(router, "alice"), ==, 0);
	g_assert_cmpuint(clawt_mailbox_depth(mailbox), ==, 1);
	g_assert_true(clawt_config_set_string(config,
		"orchestration.daily_fleet_budget_micros", "2"));
	clawt_mailbox_router_sweep(router);
	g_assert_cmpuint(clawt_mailbox_depth(mailbox), ==, 0);
	g_assert_no_error(error);
	g_clear_object(&router);
	g_clear_object(&agents);
	g_clear_object(&rooms);
	g_clear_object(&config);
	clawt_test_remove_tree(dir);
}

/* Reject malformed limits before the daemon can admit any work. */
static void
test_invalid_limits(void)
{
	const gchar *values[] = { "-1", "1.5", "9223372036854775808", "nope", NULL };
	guint i;

	for (i = 0; values[i] != NULL; i++) {
		g_autofree gchar *yaml = g_strdup_printf(
			"orchestration:\n  daily_fleet_budget_micros: '%s'\n", values[i]);
		g_autoptr(GError) error = NULL;
		g_autoptr(ClawtConfig) config = clawt_config_load_from_string(yaml, &error);
		g_assert_no_error(error);
		g_assert_false(clawt_config_validate(config, &error));
		g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID);
	}
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(ClawtConfig) config = clawt_config_load_from_string(
			"agents:\n  - id: bad\n    daily_agent_budget_micros: -1\n", &error);
		g_assert_no_error(error);
		g_assert_false(clawt_config_validate(config, &error));
		g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID);
	}
}

/* ENOTDIR and EACCES are unavailable accounting, never proof of zero usage. */
static void
test_unavailable_paths(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("clawt-daily-unavailable-XXXXXX", NULL);
	g_autofree gchar *yaml = g_strdup_printf(
		"daemon:\n  state_dir: '%s'\norchestration:\n"
		"  daily_fleet_budget_micros: 1\nagents:\n  - id: alice\n", dir);
	g_autoptr(GError) error = NULL;
	g_autoptr(ClawtConfig) config = clawt_config_load_from_string(yaml, &error);
	g_autoptr(ClawtAgentManager) agents = clawt_agent_manager_new(config);
	g_autoptr(ClawtRoomManager) rooms = clawt_room_manager_new(NULL);
	g_autoptr(ClawtMailboxRouter) router = clawt_mailbox_router_new(agents, rooms, NULL);
	g_autofree gchar *state = clawt_config_agent_state_dir(config, "alice");
	g_autofree gchar *sessions = g_build_filename(state, "sessions", NULL);
	g_autofree gchar *archive = g_build_filename(state, "sessions.reset-test", NULL);
	gint64 now = g_get_real_time() / G_USEC_PER_SEC;

	g_assert_no_error(error);
	/* A genuinely absent database still means this agent has spent nothing. */
	g_assert_true(clawt_mailbox_router_check_daily_budget(router, "alice", now, &error));
	g_assert_cmpint(g_mkdir_with_parents(state, 0700), ==, 0);
	g_assert_true(g_file_set_contents(sessions, "not a directory", -1, &error));
	g_assert_false(clawt_mailbox_router_check_daily_budget(router, "alice", now, &error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED);
	g_clear_error(&error);
	g_assert_false(clawt_mailbox_router_check_daily_budget(router, "alice", now, NULL));
	g_assert_cmpint(g_unlink(sessions), ==, 0);
	g_assert_cmpint(g_mkdir(sessions, 0700), ==, 0);
	if (geteuid() != 0) {
		gboolean admitted;

		g_assert_cmpint(g_chmod(sessions, 0000), ==, 0);
		admitted = clawt_mailbox_router_check_daily_budget(router, "alice", now, &error);
		/* Restore access before assertions so failed tests retain readable evidence. */
		g_assert_cmpint(g_chmod(sessions, 0700), ==, 0);
		g_assert_false(admitted);
		g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED);
		g_clear_error(&error);
	} else {
		g_test_message("Permission denial check skipped for privileged uid 0");
	}
	/* Archives use the same strict read path as the current sessions directory. */
	g_assert_true(g_file_set_contents(archive, "not a directory", -1, &error));
	g_assert_false(clawt_mailbox_router_check_daily_budget(router, "alice", now, &error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED);
	g_clear_error(&error);
	g_clear_object(&router);
	g_clear_object(&agents);
	g_clear_object(&rooms);
	g_clear_object(&config);
	clawt_test_remove_tree(dir);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/daily-budgets/persisted-limits", test_daily);
	g_test_add_func("/daily-budgets/delivery", test_delivery);
	g_test_add_func("/daily-budgets/invalid-limits", test_invalid_limits);
	g_test_add_func("/daily-budgets/unavailable-paths", test_unavailable_paths);
	return g_test_run();
}
