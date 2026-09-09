/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <clawtilla.h>
#include "clawt-test-util.h"

/* The callback checks capacity from a second database connection before any
 * result has been saved, proving the whole batch precedes the first effect. */
typedef struct {
	ClawtTriggerStore *observer;
	guint calls;
} Runs;

static const gchar *
run_recipient(const gchar *id, const gchar *agent, const gchar *prompt,
	gpointer user_data, GError **error)
{
	Runs *runs = user_data;
	g_assert_cmpstr(id, ==, "ci");
	g_assert_nonnull(prompt);
	if (runs->calls == 0)
		g_assert_cmpuint(clawt_trigger_store_count_unfinished(runs->observer, id), ==, 3);
	runs->calls++;
	if (g_str_equal(agent, "missing")) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND, "agent not found");
		return NULL;
	}
	return agent;
}

static void
test_fanout(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("clawt-fanout-XXXXXX", NULL);
	g_autofree gchar *path = g_build_filename(dir, "triggers.db", NULL);
	g_autoptr(ClawtConfig) config = clawt_config_new();
	g_autoptr(ClawtTriggerStore) store = NULL;
	g_autoptr(ClawtTriggerStore) observer = NULL;
	g_autoptr(ClawtTriggerEvent) event = clawt_trigger_event_new(CLAWT_TRIGGER_PROVIDER_GENERIC, "push", "parent");
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) results = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_auto(GStrv) recipients = NULL;
	const gchar *additional[] = { "builder", "missing", "reviewer", "reviewer", " ", NULL };
	const gchar *too_many[] = { "two", "three", "four", "five", NULL };
	ClawtTrigger *trigger;
	Runs runs;

	trigger = clawt_config_add_trigger(config, "ci", &error);
	g_assert_no_error(error);
	clawt_trigger_set_string(trigger, "agent", "builder");
	clawt_trigger_set_string_list(trigger, "agents", additional);
	recipients = clawt_trigger_get_recipients(trigger);
	g_assert_cmpuint(g_strv_length(recipients), ==, 3);
	g_assert_cmpstr(recipients[0], ==, "builder");
	g_assert_cmpstr(recipients[1], ==, "missing");
	g_assert_cmpstr(recipients[2], ==, "reviewer");
	store = clawt_trigger_store_new(path, &error);
	observer = clawt_trigger_store_new(path, &error);
	g_assert_no_error(error);
	runs.observer = observer;
	runs.calls = 0;
	results = clawt_trigger_dispatch(trigger, store, event, "parent", run_recipient, &runs, &error);
	g_assert_no_error(error);
	g_assert_nonnull(results);
	g_assert_cmpuint(runs.calls, ==, 3);
	g_assert_cmpuint(json_array_get_length(json_node_get_array(results)), ==, 3);
	g_assert_true(json_object_has_member(json_array_get_object_element(json_node_get_array(results), 1), "error"));
	g_assert_cmpuint(clawt_trigger_store_count_unfinished(store, "ci"), ==, 2);
	g_assert_cmpuint(clawt_trigger_store_recent_count(store, "ci", 60), ==, 1);
	rows = clawt_trigger_store_list_deliveries(store, "ci", 10);
	g_assert_cmpuint(rows->len, ==, 3);
	g_assert_cmpstr(g_hash_table_lookup(g_ptr_array_index(rows, 0), "agent"), ==, "reviewer");

	/* Retry after reopening cannot re-run the successful siblings. */
	g_clear_object(&store);
	store = clawt_trigger_store_new(path, &error);
	g_clear_pointer(&results, json_node_unref);
	results = clawt_trigger_dispatch(trigger, store, event, "parent", run_recipient, &runs, &error);
	g_assert_null(results);
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_cmpuint(runs.calls, ==, 3);

	/* Three recipients do not fit two free slots; no receipt is consumed. */
	results = clawt_trigger_dispatch(trigger, store, event, "later", run_recipient, &runs, &error);
	g_assert_null(results);
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT);
	g_clear_error(&error);
	g_assert_false(clawt_trigger_store_seen_delivery(store, "ci", "later"));
	clawt_trigger_store_finish(store, "builder");
	clawt_trigger_store_finish(store, "reviewer");
	g_assert_cmpuint(clawt_trigger_store_count_unfinished(store, "ci"), ==, 0);

	/* Duplicate refusal still applies after capacity becomes available. */
	results = clawt_trigger_dispatch(trigger, store, event, "parent", run_recipient, &runs, &error);
	g_assert_null(results);
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_cmpuint(runs.calls, ==, 3);
	clawt_trigger_set_string_list(trigger, "agents", too_many);
	results = clawt_trigger_dispatch(trigger, store, event, "huge", run_recipient, &runs, &error);
	g_assert_null(results);
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT);
	g_clear_error(&error);
	g_assert_false(clawt_trigger_store_seen_delivery(store, "ci", "huge"));
	g_clear_object(&observer);
	g_clear_object(&store);
	clawt_test_remove_tree(dir);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/trigger/fanout", test_fanout);
	return g_test_run();
}
