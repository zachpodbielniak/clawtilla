/*
 * daemon-usage.c - What the container computers are using, and their limits
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Three things, all about container computers' resources:
 *
 *  - the sampler: every CLAWT_USAGE_INTERVAL_SECONDS, one
 *    `podman stats --no-stream` per podman connection that has a running
 *    container computer on it, on a worker thread. The samples are left
 *    on each ClawtContainerComputer, where agent.list reads them -- so a
 *    client asking every second costs nothing, and a fleet of thirty
 *    costs one podman call a half minute.
 *  - the OOM watch: podman's own "oom" event, through the pod bridge, and
 *    the cgroup's oom_kill counter at each sample (which also sees a
 *    compiler killed inside a machine that itself lived on). Either
 *    becomes one incident naming the limit.
 *  - what agent.list and team.list say about the limits: each key's
 *    effective value and where it came from, what is refused, how the
 *    last change landed.
 */

#include "clawtilla.h"

#include <string.h>

#include "core/clawt-daemon.h"
#include "core/clawt-daemon-private.h"

/*
 * How often the sampler runs. Thirty seconds: often enough that "sitting
 * at its memory limit" means now, rarely enough that podman stats is
 * never a load of its own.
 */
#define CLAWT_USAGE_INTERVAL_SECONDS (30)

/* How long one podman stats or inspect may take before it is abandoned. */
#define USAGE_PODMAN_SECONDS (20)

/* ── One sampling pass ──────────────────────────────────────────── */

/* One machine to sample, and what the worker found for it. */
typedef struct {
	ClawtContainerComputer *computer;
	gchar                  *connection;
	gchar                  *name;
	gchar                  *cgroup_path;    /* known, or found by the worker */
	gboolean                cgroup_found;   /* the worker found it this pass */
	ClawtContainerUsage    *previous;
	ClawtContainerUsage    *usage;          /* NULL: not in the sample */
	gboolean                sampled;        /* podman answered for its connection */
} Probe;

static void
probe_free(
	gpointer data
){
	Probe *probe = data;

	g_object_unref(probe->computer);
	g_free(probe->connection);
	g_free(probe->name);
	g_free(probe->cgroup_path);
	clawt_container_usage_free(probe->previous);
	clawt_container_usage_free(probe->usage);
	g_free(probe);
}

/*
 * A connection on this host: the default socket or any unix:// one. Only
 * there is the container's cgroup a file clawtilla can read.
 */
static gboolean
connection_is_local(
	const gchar *connection
){
	return connection == NULL || *connection == '\0' ||
	       g_str_has_prefix(connection, "unix://");
}

/* Runs @argv, giving back what it printed, or NULL when it failed. */
static gchar *
run_quietly(
	GStrv argv
){
	g_autofree gchar *out = NULL;
	g_autoptr(GError) error = NULL;
	gint status = -1;

	if (!clawt_run_argv_bounded((const gchar * const *)argv,
	                            USAGE_PODMAN_SECONDS, &status, &out, NULL,
	                            &error)) {
		g_debug("usage: %s: %s", argv[0], error->message);
		return NULL;
	}

	if (status != 0) {
		g_debug("usage: `%s %s` exited %d", argv[0],
		        argv[1] != NULL ? argv[1] : "", status);
		return NULL;
	}

	return g_steal_pointer(&out);
}

/*
 * The worker: one stats call per connection, one cgroup inspect for the
 * machines on a local connection whose cgroup is not known yet, and a
 * read of each one's memory.events.
 */
static void
sample_worker(
	GTask        *task,
	gpointer      source,
	gpointer      data,
	GCancellable *cancellable
){
	GPtrArray *probes = data;
	g_autofree gchar *podman = NULL;
	g_autoptr(GHashTable) done = NULL;
	gint64 now = g_get_real_time();
	guint i;

	(void)source;
	(void)cancellable;

	podman = clawt_container_limits_find_podman();

	if (podman == NULL) {
		g_task_return_boolean(task, FALSE);
		return;
	}

	done = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	for (i = 0; i < probes->len; i++) {
		Probe *first = g_ptr_array_index(probes, i);
		const gchar *connection = first->connection != NULL
		                          ? first->connection : "";
		g_auto(GStrv) argv = NULL;
		g_autofree gchar *text = NULL;
		g_autoptr(GHashTable) previous = NULL;
		g_autoptr(GPtrArray) samples = NULL;
		g_autoptr(GPtrArray) unknown = NULL;
		guint j;

		if (!g_hash_table_add(done, g_strdup(connection)))
			continue;

		/* The last samples on this connection, for the CPU rate. */
		previous = g_hash_table_new(g_str_hash, g_str_equal);

		for (j = i; j < probes->len; j++) {
			Probe *probe = g_ptr_array_index(probes, j);

			if (g_strcmp0(probe->connection, first->connection) == 0 &&
			    probe->previous != NULL)
				g_hash_table_replace(previous, probe->name, probe->previous);
		}

		argv = clawt_container_usage_stats_argv(podman, first->connection);
		text = run_quietly(argv);

		if (text == NULL)
			continue;

		samples = clawt_container_usage_parse(text, previous, now);
		unknown = g_ptr_array_new();

		for (j = i; j < probes->len; j++) {
			Probe *probe = g_ptr_array_index(probes, j);
			guint k;

			if (g_strcmp0(probe->connection, first->connection) != 0)
				continue;

			probe->sampled = TRUE;

			for (k = 0; k < samples->len; k++) {
				ClawtContainerUsage *usage = g_ptr_array_index(samples, k);

				if (g_strcmp0(usage->name, probe->name) == 0) {
					probe->usage = clawt_container_usage_copy(usage);
					break;
				}
			}

			if (probe->usage != NULL && probe->cgroup_path == NULL &&
			    connection_is_local(probe->connection))
				g_ptr_array_add(unknown, probe->name);
		}

		/* Where the new ones keep their cgroup: once, then remembered. */
		if (unknown->len > 0) {
			g_auto(GStrv) inspect = NULL;
			g_autofree gchar *paths_text = NULL;
			g_autoptr(GHashTable) paths = NULL;

			g_ptr_array_add(unknown, NULL);
			inspect = clawt_container_usage_cgroup_argv(
				podman, first->connection,
				(const gchar * const *)unknown->pdata);
			paths_text = run_quietly(inspect);
			paths = clawt_container_usage_parse_cgroups(paths_text);

			for (j = i; j < probes->len; j++) {
				Probe *probe = g_ptr_array_index(probes, j);
				const gchar *path;

				if (g_strcmp0(probe->connection, first->connection) != 0 ||
				    probe->cgroup_path != NULL)
					continue;

				path = g_hash_table_lookup(paths, probe->name);

				if (path != NULL) {
					probe->cgroup_path = g_strdup(path);
					probe->cgroup_found = TRUE;
				}
			}
		}

		for (j = i; j < probes->len; j++) {
			Probe *probe = g_ptr_array_index(probes, j);

			if (probe->usage != NULL && probe->cgroup_path != NULL &&
			    connection_is_local(probe->connection))
				probe->usage->oom_kills =
					clawt_container_usage_read_oom_kills(NULL,
					                                     probe->cgroup_path);
		}
	}

	g_task_return_boolean(task, TRUE);
}

/*
 * An OOM kill in @computer's machine, as an incident naming the limit --
 * once, however many ways it was heard about.
 */
static void
report_oom(
	ClawtDaemon            *self,
	ClawtContainerComputer *computer,
	gint64                  killed
){
	g_autofree gchar *memory = NULL;
	g_autofree gchar *reason = NULL;
	const gchar *agent_id;

	if (!clawt_container_computer_claim_oom_report(computer,
	                                               g_get_real_time()))
		return;

	agent_id = clawt_computer_get_agent_id(CLAWT_COMPUTER(computer));
	memory = clawt_container_computer_get_memory_limit(computer);
	reason = clawt_container_usage_oom_reason(
		memory, clawt_container_computer_get_name(computer), killed);

	g_warning("agent %s: %s", agent_id != NULL ? agent_id : "?", reason);

	if (self->incidents != NULL && agent_id != NULL)
		clawt_incident_desk_raise(self->incidents,
		                          CLAWT_INCIDENT_OUT_OF_MEMORY, agent_id,
		                          NULL, NULL, reason);

	if (agent_id != NULL)
		clawt_event_bus_emit(self->bus, "agent.changed", agent_id);
}

/* On the main loop: the samples onto their computers, and what is news. */
static void
sample_done(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	ClawtDaemon *self = CLAWT_DAEMON(user_data);
	GPtrArray *probes = g_task_get_task_data(G_TASK(result));
	guint i;

	(void)source;

	self->usage_in_flight = FALSE;

	if (!g_task_propagate_boolean(G_TASK(result), NULL)) {
		g_object_unref(self);
		return;
	}

	for (i = 0; i < probes->len; i++) {
		Probe *probe = g_ptr_array_index(probes, i);
		const gchar *agent_id =
			clawt_computer_get_agent_id(CLAWT_COMPUTER(probe->computer));
		gboolean pressure_changed = FALSE;
		gint64 killed;

		/* podman did not answer for its connection: keep the last. */
		if (!probe->sampled)
			continue;

		if (probe->cgroup_found)
			clawt_container_computer_set_cgroup_path(probe->computer,
			                                         probe->cgroup_path);

		killed = clawt_container_computer_take_usage(probe->computer,
		                                             probe->usage,
		                                             &pressure_changed);

		if (killed > 0)
			report_oom(self, probe->computer, killed);

		if (!pressure_changed)
			continue;

		if (clawt_container_computer_get_memory_pressure(probe->computer)) {
			g_autofree gchar *memory =
				clawt_container_computer_get_memory_limit(probe->computer);
			g_autofree gchar *used = (probe->usage != NULL)
				? clawt_container_usage_format_bytes(probe->usage->memory_used)
				: g_strdup("?");

			g_warning("agent %s: its computer is at its memory limit: %s "
			          "of %s used. Past it the kernel kills a process "
			          "inside; raise computer.container.memory or ease off",
			          agent_id != NULL ? agent_id : "?", used,
			          memory != NULL ? memory : "?");
		} else {
			g_message("agent %s: its computer is no longer at its memory "
			          "limit", agent_id != NULL ? agent_id : "?");
		}

		if (agent_id != NULL)
			clawt_event_bus_emit(self->bus, "agent.changed", agent_id);
	}

	g_object_unref(self);
}

/*
 * The timer: every running container computer, once each (a shared one
 * is on several agents), handed to the worker. Skipped while a pass is
 * still out, so a podman that has gone slow is never asked twice at once.
 */
static gboolean
on_usage_tick(
	gpointer user_data
){
	ClawtDaemon *self = CLAWT_DAEMON(user_data);
	g_autoptr(GPtrArray) probes = NULL;
	g_autoptr(GHashTable) seen = NULL;
	g_autoptr(GTask) task = NULL;
	GPtrArray *agents;
	guint i;

	if (self->usage_in_flight || self->agents == NULL)
		return G_SOURCE_CONTINUE;

	agents = clawt_agent_manager_list(self->agents);
	probes = g_ptr_array_new_with_free_func(probe_free);
	seen = g_hash_table_new(g_direct_hash, g_direct_equal);

	for (i = 0; agents != NULL && i < agents->len; i++) {
		ClawtComputer *computer =
			clawt_agent_get_computer(g_ptr_array_index(agents, i));
		ClawtContainerComputer *container;
		Probe *probe;

		if (computer == NULL || !CLAWT_IS_CONTAINER_COMPUTER(computer) ||
		    !g_hash_table_add(seen, computer))
			continue;

		container = CLAWT_CONTAINER_COMPUTER(computer);

		/* Stopped: what it last said no longer holds. */
		if (clawt_computer_get_state(computer) !=
		    CLAWT_COMPUTER_STATE_RUNNING) {
			(void)clawt_container_computer_take_usage(container, NULL, NULL);
			continue;
		}

		probe = g_new0(Probe, 1);
		probe->computer = g_object_ref(container);
		probe->connection =
			g_strdup(clawt_container_computer_get_connection(container));
		probe->name = g_strdup(clawt_container_computer_get_name(container));
		probe->cgroup_path = clawt_container_computer_get_cgroup_path(container);
		probe->previous = clawt_container_computer_get_usage(container);
		g_ptr_array_add(probes, probe);
	}

	if (probes->len == 0)
		return G_SOURCE_CONTINUE;

	self->usage_in_flight = TRUE;

	/* Completed on the daemon's own context, which is the one running. */
	if (self->main_context != NULL)
		g_main_context_push_thread_default(self->main_context);

	task = g_task_new(NULL, NULL, sample_done, g_object_ref(self));

	if (self->main_context != NULL)
		g_main_context_pop_thread_default(self->main_context);

	g_task_set_task_data(task, g_steal_pointer(&probes),
	                     (GDestroyNotify)g_ptr_array_unref);
	g_task_run_in_thread(task, sample_worker);

	return G_SOURCE_CONTINUE;
}

/*
 * podman's own word for it, through the bridge: an "oom" event on a
 * container that is one of ours. Matched by name, or by the id podman
 * gave the event (which may be the short one).
 */
static void
on_module_event(
	ClawtPodBridge *bridge,
	const gchar    *module,
	const gchar    *connection,
	const gchar    *event,
	GVariant       *data,
	gpointer        user_data
){
	ClawtDaemon *self = CLAWT_DAEMON(user_data);
	const gchar *name = NULL;
	const gchar *id = NULL;
	GPtrArray *agents;
	guint i;

	(void)bridge;
	(void)connection;

	if (g_strcmp0(module, "container") != 0 ||
	    g_strcmp0(event, "on_oom") != 0 || data == NULL ||
	    !g_variant_is_of_type(data, G_VARIANT_TYPE_VARDICT) ||
	    self->agents == NULL)
		return;

	(void)g_variant_lookup(data, "container_name", "&s", &name);
	(void)g_variant_lookup(data, "container_id", "&s", &id);

	agents = clawt_agent_manager_list(self->agents);

	for (i = 0; agents != NULL && i < agents->len; i++) {
		ClawtComputer *computer =
			clawt_agent_get_computer(g_ptr_array_index(agents, i));
		ClawtContainerComputer *container;
		const gchar *own_id;

		if (computer == NULL || !CLAWT_IS_CONTAINER_COMPUTER(computer))
			continue;

		container = CLAWT_CONTAINER_COMPUTER(computer);
		own_id = clawt_container_computer_get_container_id(container);

		if ((name != NULL && *name != '\0' &&
		     g_strcmp0(name, clawt_container_computer_get_name(container)) == 0) ||
		    (id != NULL && *id != '\0' && own_id != NULL &&
		     g_str_has_prefix(own_id, id))) {
			report_oom(self, container, 0);
			return;
		}
	}
}

void
clawt_daemon_usage_start(
	ClawtDaemon *self
){
	GSource *source;

	g_return_if_fail(CLAWT_IS_DAEMON(self));

	if (self->usage_source_id != 0)
		return;

	/*
	 * On the daemon's own context, as the sweep is: g_timeout_add_*()
	 * would use the default one, which an embedded daemon does not run.
	 */
	source = g_timeout_source_new_seconds(CLAWT_USAGE_INTERVAL_SECONDS);
	g_source_set_callback(source, on_usage_tick, self, NULL);
	self->usage_source_id = g_source_attach(source, self->main_context);
	g_source_unref(source);

	if (self->pod_bridge != NULL && self->usage_event_handler == 0)
		self->usage_event_handler =
			g_signal_connect(self->pod_bridge, "module-event",
			                 G_CALLBACK(on_module_event), self);
}

void
clawt_daemon_usage_stop(
	ClawtDaemon *self
){
	g_return_if_fail(CLAWT_IS_DAEMON(self));

	if (self->usage_source_id != 0) {
		GSource *source = g_main_context_find_source_by_id(
			self->main_context, self->usage_source_id);

		if (source != NULL)
			g_source_destroy(source);

		self->usage_source_id = 0;
	}

	if (self->usage_event_handler != 0 && self->pod_bridge != NULL)
		g_signal_handler_disconnect(self->pod_bridge,
		                            self->usage_event_handler);

	self->usage_event_handler = 0;
}

/* ── What agent.list and team.list say ──────────────────────────── */

/* The host's size, read once: it does not change under a running daemon. */
static void
host_size(
	guint  *cpus,
	gint64 *memory
){
	static gsize once = 0;
	static guint host_cpus = 0;
	static gint64 host_memory = 0;

	if (g_once_init_enter(&once)) {
		host_cpus = clawt_host_cpu_count();
		host_memory = clawt_host_memory_bytes();
		g_once_init_leave(&once, 1);
	}

	*cpus = host_cpus;
	*memory = host_memory;
}

void
clawt_daemon_add_container_limits(
	JsonBuilder *builder,
	ClawtAgent  *agent
){
	ClawtAgentConfig *config = clawt_agent_get_config(agent);
	ClawtComputer *computer = clawt_agent_get_computer(agent);
	g_autoptr(GPtrArray) refusals = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(ClawtContainerLimits) applied = NULL;
	guint host_cpus;
	gint64 host_memory;
	guint i;

	host_size(&host_cpus, &host_memory);
	applied = clawt_computer_factory_limits_checked(config, host_cpus,
	                                                refusals);

	json_builder_set_member_name(builder, "container_limits");
	json_builder_begin_object(builder);

	/*
	 * Each key: what applies (empty when refused or unset), what was
	 * written and where, and the agent's own value -- the field a client
	 * edits, so saving it back never pins an inherited value on the agent.
	 */
	for (i = 0; i < CLAWT_CONTAINER_LIMIT_KEYS; i++) {
		const gchar *key = clawt_container_limit_key_nth(i);
		ClawtSettingSource source = CLAWT_SETTING_FROM_NONE;
		const gchar *team = NULL;
		const gchar *written = clawt_agent_config_get_container_limit(
			config, key, &source, &team);
		g_autofree gchar *effective =
			clawt_container_limits_get_key(applied, key);

		json_builder_set_member_name(builder,
		                             key + strlen("computer.container."));
		json_builder_begin_object(builder);
		clawt_daemon_add_string_member(builder, "value",
		                               effective != NULL ? effective : "");
		clawt_daemon_add_string_member(builder, "written",
		                               written != NULL ? written : "");
		clawt_daemon_add_string_member(builder, "source",
		                               clawt_setting_source_nick(source));
		if (team != NULL)
			clawt_daemon_add_string_member(builder, "team", team);
		clawt_daemon_add_string_member(
			builder, "own",
			(source == CLAWT_SETTING_FROM_AGENT && written != NULL)
			? written : "");
		json_builder_end_object(builder);
	}

	json_builder_set_member_name(builder, "refused");
	json_builder_begin_array(builder);
	for (i = 0; i < refusals->len; i++)
		json_builder_add_string_value(builder,
		                              g_ptr_array_index(refusals, i));
	json_builder_end_array(builder);

	/* For a client suggesting values from the size of the machine. */
	json_builder_set_member_name(builder, "host_cpus");
	json_builder_add_int_value(builder, host_cpus);
	json_builder_set_member_name(builder, "host_memory");
	json_builder_add_int_value(builder, host_memory);

	if (computer != NULL && CLAWT_IS_CONTAINER_COMPUTER(computer)) {
		g_autofree gchar *note = NULL;
		gint64 at = 0;
		ClawtContainerLimitsOutcome outcome =
			clawt_container_computer_get_limits_outcome(
				CLAWT_CONTAINER_COMPUTER(computer), &note, &at);

		clawt_daemon_add_string_member(
			builder, "applied", clawt_container_limits_outcome_nick(outcome));
		if (note != NULL)
			clawt_daemon_add_string_member(builder, "applied_note", note);
		json_builder_set_member_name(builder, "applied_at");
		json_builder_add_int_value(builder, at);
	}

	json_builder_end_object(builder);

	if (computer == NULL || !CLAWT_IS_CONTAINER_COMPUTER(computer))
		return;

	{
		ClawtContainerComputer *container = CLAWT_CONTAINER_COMPUTER(computer);
		g_autoptr(ClawtContainerUsage) usage =
			clawt_container_computer_get_usage(container);

		/* Only once there is a sample: none is "not measured yet". */
		if (usage == NULL)
			return;

		json_builder_set_member_name(builder, "container_usage");
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "cpu_percent");
		json_builder_add_double_value(builder, usage->cpu_percent);
		json_builder_set_member_name(builder, "memory_used");
		json_builder_add_int_value(builder, usage->memory_used);
		json_builder_set_member_name(builder, "memory_limit");
		json_builder_add_int_value(builder, usage->memory_limit);
		json_builder_set_member_name(builder, "pids");
		json_builder_add_int_value(builder, usage->pids);
		json_builder_set_member_name(builder, "sampled_at");
		json_builder_add_int_value(builder, usage->sampled_at);
		json_builder_set_member_name(builder, "at_memory_limit");
		json_builder_add_boolean_value(
			builder, clawt_container_computer_get_memory_pressure(container));
		json_builder_end_object(builder);
	}
}

GStrv
clawt_daemon_container_limit_notes(
	ClawtDaemon *self
){
	g_autoptr(GPtrArray) notes = NULL;
	g_autoptr(GHashTable) seen = NULL;
	GPtrArray *agents;
	guint host_cpus;
	gint64 host_memory;
	guint i;

	g_return_val_if_fail(CLAWT_IS_DAEMON(self), NULL);

	if (self->agents == NULL)
		return NULL;

	host_size(&host_cpus, &host_memory);
	agents = clawt_agent_manager_list(self->agents);
	notes = g_ptr_array_new_with_free_func(g_free);
	seen = g_hash_table_new(g_direct_hash, g_direct_equal);

	for (i = 0; agents != NULL && i < agents->len; i++) {
		ClawtAgent *agent = g_ptr_array_index(agents, i);
		ClawtAgentConfig *config = clawt_agent_get_config(agent);
		ClawtComputer *computer = clawt_agent_get_computer(agent);
		gint type = clawt_agent_config_get_enum(config, "computer.type");
		g_autoptr(GPtrArray) refusals = NULL;
		g_autoptr(ClawtContainerLimits) applied = NULL;
		guint j;

		if (type != CLAWT_COMPUTER_CONTAINER &&
		    type != CLAWT_COMPUTER_DISTROBOX)
			continue;

		/* A refused value: said for each agent it reaches. */
		refusals = g_ptr_array_new_with_free_func(g_free);
		applied = clawt_computer_factory_limits_checked(config, host_cpus,
		                                                refusals);

		for (j = 0; j < refusals->len; j++)
			g_ptr_array_add(notes, g_strdup_printf(
				"agent %s: %s", clawt_agent_get_id(agent),
				(const gchar *)g_ptr_array_index(refusals, j)));

		/* The machine's own state: said once, under its first agent. */
		if (computer == NULL || !CLAWT_IS_CONTAINER_COMPUTER(computer) ||
		    !g_hash_table_add(seen, computer))
			continue;

		{
			ClawtContainerComputer *container =
				CLAWT_CONTAINER_COMPUTER(computer);
			g_autofree gchar *note = NULL;

			if (clawt_container_computer_get_limits_outcome(container, &note,
			                                                NULL) ==
			    CLAWT_CONTAINER_LIMITS_RESTART_REQUIRED &&
			    clawt_computer_get_state(computer) ==
			    CLAWT_COMPUTER_STATE_RUNNING)
				g_ptr_array_add(notes, g_strdup_printf(
					"agent %s: %s", clawt_agent_get_id(agent), note));

			if (clawt_container_computer_get_memory_pressure(container)) {
				g_autoptr(ClawtContainerUsage) usage =
					clawt_container_computer_get_usage(container);
				g_autofree gchar *used = (usage != NULL)
					? clawt_container_usage_format_bytes(usage->memory_used)
					: g_strdup("?");

				g_ptr_array_add(notes, g_strdup_printf(
					"agent %s is at its memory limit: %s of %s used. Past "
					"it the kernel kills a process inside its computer",
					clawt_agent_get_id(agent), used,
					clawt_container_limits_get_memory(applied) != NULL
					? clawt_container_limits_get_memory(applied) : "?"));
			}
		}
	}

	if (notes->len == 0)
		return NULL;

	g_ptr_array_add(notes, NULL);

	return (GStrv)g_ptr_array_free(g_steal_pointer(&notes), FALSE);
}
