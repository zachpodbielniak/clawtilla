/*
 * daemon-project.c - Projects: what work belongs to, and what it costs
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * - Placing a task: which project it belongs to (clawt_project_resolve()),
 *   the sentence its assignee is told, the project's reviewer when the
 *   delegation named none, and whether the project may take new work.
 * - A project's month: the spend of its tasks made this calendar month,
 *   summed from every agent's usage ledger, cached for a minute because
 *   every delegation into the project asks.
 * - The budget: a warning to the operator once a month at
 *   budget_warn_percent, and -- with budget_hard_stop -- new work refused
 *   past it, as the spending caps refuse (CLAWT_ERROR_LOOP_LIMIT).
 * - project.list / project.create / project.set / project.remove.
 */

#include "clawtilla.h"
#include "core/clawt-daemon-private.h"

/* How long a project's month of spend is trusted before it is re-read. */
#define PROJECT_SPEND_TTL_SECONDS (60)

typedef struct {
	gint64 micros;
	gint64 read_at;
	gint64 month;
} ProjectSpend;

/*
 * Keeps @micros as @project_id's spend for this month, as read at @now.
 * Where every read lands; a test uses it to stand in for the ledgers.
 */
void
clawt_daemon_project_remember_spend(
	ClawtDaemon *self,
	const gchar *project_id,
	gint64       micros,
	gint64       now
){
	ProjectSpend *cached = g_new0(ProjectSpend, 1);

	g_return_if_fail(CLAWT_IS_DAEMON(self));

	if (self->project_spend == NULL)
		self->project_spend = g_hash_table_new_full(g_str_hash, g_str_equal,
		                                            g_free, g_free);

	cached->micros = micros;
	cached->read_at = now;
	cached->month = clawt_project_month_start(now);
	g_hash_table_replace(self->project_spend, g_strdup(project_id), cached);
}

gint64
clawt_daemon_project_spend(
	ClawtDaemon *self,
	const gchar *project_id,
	gint64       now
){
	g_autoptr(GPtrArray) tasks = NULL;
	g_autoptr(GHashTable) ids = NULL;
	g_autoptr(GHashTable) spend = NULL;
	GHashTableIter iter;
	gpointer value;
	ProjectSpend *cached;
	GPtrArray *agents;
	gint64 month;
	gint64 total = 0;
	guint i;

	g_return_val_if_fail(CLAWT_IS_DAEMON(self), 0);

	if (project_id == NULL || self->task_store == NULL)
		return 0;

	month = clawt_project_month_start(now);

	if (self->project_spend == NULL)
		self->project_spend = g_hash_table_new_full(g_str_hash, g_str_equal,
		                                            g_free, g_free);

	cached = g_hash_table_lookup(self->project_spend, project_id);

	if (cached != NULL && cached->month == month &&
	    now - cached->read_at < PROJECT_SPEND_TTL_SECONDS)
		return cached->micros;

	/*
	 * The month's tasks, then what each agent's turns on them cost.  A
	 * task made late last month that ran into this one counts towards
	 * last month: a month is the work started in it.
	 */
	tasks = clawt_task_store_project_tasks(self->task_store, project_id,
	                                       month, NULL);
	ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	spend = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	for (i = 0; tasks != NULL && i < tasks->len; i++)
		g_hash_table_add(ids, g_strdup(clawt_task_get_id(
			g_ptr_array_index(tasks, i))));

	agents = clawt_agent_manager_list(self->agents);

	for (i = 0; g_hash_table_size(ids) > 0 && agents != NULL &&
	            i < agents->len; i++) {
		const gchar *agent_id = clawt_agent_get_id(
			g_ptr_array_index(agents, i));
		g_autofree gchar *state_dir = clawt_config_agent_state_dir(
			self->config, agent_id);
		g_autofree gchar *db_path = NULL;

		if (state_dir == NULL)
			continue;

		db_path = clawt_usage_database_path(state_dir);
		clawt_usage_task_spend(db_path, ids, spend, NULL);
	}

	g_hash_table_iter_init(&iter, spend);
	while (g_hash_table_iter_next(&iter, NULL, &value))
		total += (gint64)GPOINTER_TO_SIZE(value);

	clawt_daemon_project_remember_spend(self, project_id, total, now);

	return total;
}

/*
 * Once a month, at the warning line: a notification, so the operator
 * hears before the stop rather than from it.
 */
static void
warn_once(
	ClawtDaemon      *self,
	ClawtProjectSpec *spec,
	gint64            micros,
	gint64            now
){
	g_autoptr(GDateTime) at = g_date_time_new_from_unix_local(now);
	g_autofree gchar *month = g_date_time_format(at, "%Y-%m");
	g_autofree gchar *key = g_strdup_printf("%s:%s", spec->id, month);
	g_autofree gchar *spent = clawt_usage_format_cost(micros);
	g_autofree gchar *body = NULL;
	g_autoptr(ClawtNotification) notification = NULL;

	if (self->project_warned == NULL)
		self->project_warned = g_hash_table_new_full(g_str_hash, g_str_equal,
		                                             g_free, NULL);

	if (g_hash_table_contains(self->project_warned, key))
		return;

	g_hash_table_add(self->project_warned, g_steal_pointer(&key));

	body = g_strdup_printf(
		"Project %s has spent %s of its $%.2f budget this month%s.",
		spec->id, spent, spec->monthly_budget_usd,
		spec->budget_hard_stop
			? "; new work is refused once it is all spent" : "");
	g_message("projects: %s", body);

	if (self->notifier == NULL)
		return;

	notification = clawt_notification_new(CLAWT_NOTIFY_EVENTS_BUDGET, NULL,
	                                      NULL, "project budget", body);
	clawt_notifier_notify(self->notifier, notification);
}

gboolean
clawt_daemon_project_admit(
	ClawtDaemon  *self,
	const gchar  *project_id,
	GError      **error
){
	g_autoptr(ClawtProjectSpec) spec = NULL;
	gint64 now = g_get_real_time() / G_USEC_PER_SEC;
	gint64 micros;
	gint64 budget;

	g_return_val_if_fail(CLAWT_IS_DAEMON(self), FALSE);

	if (project_id == NULL)
		return TRUE;

	spec = clawt_config_get_project(self->config, project_id);

	if (spec == NULL || spec->monthly_budget_usd <= 0)
		return TRUE;

	micros = clawt_daemon_project_spend(self, project_id, now);
	budget = (gint64)(spec->monthly_budget_usd * 1000000.0);

	if (micros * 100 >= budget * spec->budget_warn_percent)
		warn_once(self, spec, micros, now);

	if (spec->budget_hard_stop && micros >= budget) {
		g_autofree gchar *spent = clawt_usage_format_cost(micros);

		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT,
		            "project %s has spent %s of its $%.2f budget this "
		            "month, and it stops there (budget_hard_stop); raise "
		            "the budget or wait for next month", project_id, spent,
		            spec->monthly_budget_usd);
		return FALSE;
	}

	return TRUE;
}

gboolean
clawt_daemon_project_place(
	ClawtDaemon  *self,
	ClawtTask    *task,
	const gchar  *named,
	const gchar  *repository,
	gboolean      review_given,
	GError      **error
){
	g_autoptr(ClawtProjectSpec) spec = NULL;
	g_autofree gchar *brief = NULL;
	const gchar *parent_project = NULL;

	g_return_val_if_fail(CLAWT_IS_DAEMON(self), FALSE);
	g_return_val_if_fail(task != NULL, FALSE);

	if (clawt_task_get_parent_id(task) != NULL) {
		ClawtTask *parent = clawt_task_manager_get(
			self->tasks, clawt_task_get_parent_id(task));

		parent_project = (parent != NULL) ? clawt_task_get_project(parent)
		                                  : NULL;
	}

	if (repository == NULL)
		repository = clawt_task_get_checkout_repo(task);

	spec = clawt_project_resolve(self->config, named, parent_project,
	                             repository, error);

	if (spec == NULL)
		return (error == NULL || *error == NULL);

	if (!clawt_daemon_project_admit(self, spec->id, error))
		return FALSE;

	brief = clawt_project_spec_brief(spec);
	clawt_task_set_project(task, spec->id, brief);

	/*
	 * The project's reviewer, when the delegation did not name one: the
	 * project's rule beats the assignee's habit.  Resolved as any other
	 * spelling is, so it is never the assignee.
	 */
	if (!review_given && spec->review_by != NULL) {
		g_autoptr(GError) review_error = NULL;
		g_autofree gchar *reviewer = clawt_task_resolve_reviewer(
			self->config, spec->review_by, clawt_task_get_origin(task),
			clawt_task_get_assignee(task), &review_error);

		if (review_error != NULL)
			g_warning("project %s: review_by '%s' names nobody usable: %s",
			          spec->id, spec->review_by, review_error->message);
		else
			clawt_task_set_reviewer(task, reviewer);
	}

	return TRUE;
}

/* clawtilla_delegate's hook. */
gboolean
clawt_daemon_project_place_for_tools(
	ClawtTask    *task,
	const gchar  *named,
	gboolean      review_given,
	gpointer      user_data,
	GError      **error
){
	return clawt_daemon_project_place(user_data, task, named, NULL,
	                                  review_given, error);
}

/* ── IPC ─────────────────────────────────────────────────────────── */

static void
add_project_object(
	ClawtDaemon      *self,
	JsonBuilder      *builder,
	ClawtProjectSpec *spec,
	gint64            now
){
	gint64 micros = clawt_daemon_project_spend(self, spec->id, now);
	guint i;

	json_builder_begin_object(builder);
	clawt_daemon_add_string_member(builder, "id", spec->id);
	clawt_daemon_add_string_member(builder, "name",
		spec->name != NULL ? spec->name : spec->id);
	clawt_daemon_add_string_member(builder, "directory", spec->directory);
	clawt_daemon_add_string_member(builder, "goal", spec->goal);
	clawt_daemon_add_string_member(builder, "review_by", spec->review_by);

	json_builder_set_member_name(builder, "agents");
	json_builder_begin_array(builder);
	for (i = 0; spec->agents != NULL && spec->agents[i] != NULL; i++)
		json_builder_add_string_value(builder, spec->agents[i]);
	json_builder_end_array(builder);

	json_builder_set_member_name(builder, "monthly_budget_usd");
	json_builder_add_double_value(builder, spec->monthly_budget_usd);
	json_builder_set_member_name(builder, "budget_warn_percent");
	json_builder_add_int_value(builder, spec->budget_warn_percent);
	json_builder_set_member_name(builder, "budget_hard_stop");
	json_builder_add_boolean_value(builder, spec->budget_hard_stop);
	json_builder_set_member_name(builder, "month_spend_usd");
	json_builder_add_double_value(builder, (gdouble)micros / 1000000.0);
	json_builder_set_member_name(builder, "stopped");
	json_builder_add_boolean_value(builder,
		spec->budget_hard_stop && spec->monthly_budget_usd > 0 &&
		micros >= (gint64)(spec->monthly_budget_usd * 1000000.0));
	json_builder_end_object(builder);
}

/*
 * The agents' PROJECTS.org lists the projects, so a change rewrites
 * them, as team.set does for a team's description.
 */
static JsonNode *
saved_and_rendered(
	ClawtDaemon *self,
	JsonNode    *request,
	JsonBuilder *builder,
	const gchar *project_id
){
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) refusals = NULL;

	if (!clawt_config_save(self->config, &error))
		return clawt_ipc_error_new(request, error->code, error->message);

	if (self->project_spend != NULL)
		g_hash_table_remove(self->project_spend, project_id);

	refusals = clawt_daemon_render_refusals_new();
	clawt_daemon_render_all_agents_into(self, refusals);
	clawt_event_bus_emit(self->bus, "project.changed", project_id);

	json_builder_begin_object(builder);
	clawt_daemon_add_string_member(builder, "project", project_id);
	clawt_daemon_add_render_refusals(builder, refusals);
	json_builder_end_object(builder);

	return clawt_ipc_response_new(request, json_builder_get_root(builder));
}

JsonNode *
clawt_daemon_handle_project(
	ClawtDaemon  *self,
	const gchar  *kind,
	JsonNode     *request,
	JsonObject   *payload,
	gboolean     *handled
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(GError) error = NULL;
	const gchar *project_id = clawt_ipc_payload_string(payload, "project");

	*handled = TRUE;

	if (g_strcmp0(kind, "project.list") == 0) {
		g_autoptr(GPtrArray) projects = clawt_config_get_projects(
			self->config);
		gint64 now = g_get_real_time() / G_USEC_PER_SEC;
		guint i;

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "projects");
		json_builder_begin_array(builder);
		for (i = 0; i < projects->len; i++)
			add_project_object(self, builder,
			                   g_ptr_array_index(projects, i), now);
		json_builder_end_array(builder);
		json_builder_end_object(builder);

		return clawt_ipc_response_new(request, json_builder_get_root(builder));
	}

	if (g_strcmp0(kind, "project.create") == 0) {
		if (!clawt_config_add_project(self->config, project_id, &error))
			return clawt_ipc_error_new(request, error->code, error->message);

		/* The fields given with it, in one request. */
		{
			static const gchar *const fields[] = {
				"name", "directory", "goal", "review_by", "agents",
				"monthly_budget_usd", "budget_warn_percent",
				"budget_hard_stop", NULL
			};
			guint i;

			for (i = 0; fields[i] != NULL; i++) {
				const gchar *value = clawt_ipc_payload_string(payload,
				                                              fields[i]);

				if (value != NULL &&
				    !clawt_config_set_project_value(self->config, project_id,
				                                    fields[i], value,
				                                    &error)) {
					clawt_config_remove_project(self->config, project_id);
					return clawt_ipc_error_new(request, error->code,
					                           error->message);
				}
			}
		}

		return saved_and_rendered(self, request, builder, project_id);
	}

	if (g_strcmp0(kind, "project.set") == 0) {
		const gchar *key = clawt_ipc_payload_string(payload, "key");

		if (project_id == NULL || key == NULL)
			return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
			                           "project and key are both required");

		if (!clawt_config_set_project_value(
		        self->config, project_id, key,
		        clawt_ipc_payload_string(payload, "value"), &error))
			return clawt_ipc_error_new(request, error->code, error->message);

		return saved_and_rendered(self, request, builder, project_id);
	}

	if (g_strcmp0(kind, "project.remove") == 0) {
		if (!clawt_config_remove_project(self->config, project_id))
			return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
			                           "no such project");

		return saved_and_rendered(self, request, builder, project_id);
	}

	*handled = FALSE;
	return NULL;
}
