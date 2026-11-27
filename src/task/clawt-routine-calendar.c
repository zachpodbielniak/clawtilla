/*
 * clawt-routine-calendar.c - The fleet's routines laid out in time
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "task/clawt-routine-calendar.h"

#include <yaml-glib.h>

#include <string.h>

/* `daemon.calendar_max_days` and `_max_occurrences` when unset. */
#define DEFAULT_MAX_DAYS        (62)
#define DEFAULT_MAX_OCCURRENCES (5000)

/* `routine.upcoming` with no limit. */
#define DEFAULT_UPCOMING (10)

/*
 * How long a time that has passed with nothing recorded is still "due"
 * rather than "unknown": the runner ticks once a minute, so a run for a
 * minute ago may simply not have been reached yet.  Two ticks, so a tick
 * that is a few seconds late is not a mystery.
 */
#define DUE_GRACE_SECONDS (120)

/* ICS lines are 75 octets at most, excluding the CRLF (RFC 5545 3.1). */
#define ICS_LINE_OCTETS (75)

struct _ClawtRoutineCalendar {
	GObject parent_instance;

	ClawtConfig        *config;   /* owned */
	ClawtRoutineRunner *runner;   /* owned, nullable */

	ClawtCalendarTaskFunc  task_func;
	gpointer               task_data;
	GDestroyNotify         task_notify;

	ClawtCalendarAgentFunc agent_func;
	gpointer               agent_data;
	GDestroyNotify         agent_notify;

	guint max_days;          /* 0: from the config */
	guint max_occurrences;   /* 0: from the config */
};

G_DEFINE_FINAL_TYPE(ClawtRoutineCalendar, clawt_routine_calendar,
                    G_TYPE_OBJECT)

/*
 * One occurrence, before it is written out.
 *
 * Strings are owned so a group can hold the same occurrence its member
 * list does without anybody's config being reloaded under it.
 */
typedef struct {
	const gchar           *source;      /* static */
	gchar                 *routine;
	gchar                 *agent;
	gchar                 *title;
	gchar                 *repeat;
	gint64                 at;          /* Unix time */
	gint64                 latest;      /* at + jitter */
	gint64                 started_at;  /* 0 when it has not started */
	gboolean               manual;      /* not on the schedule */
	gboolean               by_hand;
	gboolean               queued;
	ClawtOccurrenceStatus  status;
	const gchar           *reason;      /* static, nullable */
	gchar                 *detail;
	gchar                 *task;
	gint                   task_state;  /* -1 when there is no task */
	gchar                 *room;
} Occurrence;

static void
occurrence_free(
	Occurrence *self
){
	if (self == NULL)
		return;

	g_free(self->routine);
	g_free(self->agent);
	g_free(self->title);
	g_free(self->repeat);
	g_free(self->detail);
	g_free(self->task);
	g_free(self->room);
	g_free(self);
}

static Occurrence *
occurrence_new(
	const gchar *source,
	const gchar *routine,
	const gchar *agent,
	const gchar *title,
	const gchar *repeat,
	gint64       at,
	guint        jitter
){
	Occurrence *self = g_new0(Occurrence, 1);

	self->source = source;
	self->routine = g_strdup(routine);
	self->agent = g_strdup(agent);
	self->title = g_strdup(title);
	self->repeat = g_strdup(repeat);
	self->at = at;
	self->latest = at + (gint64)jitter;
	self->task_state = -1;

	return self;
}

/* Soonest first; one time's runs in a stable order. */
static gint
occurrence_compare(
	gconstpointer a,
	gconstpointer b
){
	const Occurrence *left = *(const Occurrence *const *)a;
	const Occurrence *right = *(const Occurrence *const *)b;
	gint by_source;

	if (left->at != right->at)
		return (left->at < right->at) ? -1 : 1;

	by_source = g_strcmp0(left->source, right->source);
	if (by_source != 0)
		return by_source;

	return g_strcmp0(left->routine, right->routine);
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

static void
clawt_routine_calendar_finalize(
	GObject *object
){
	ClawtRoutineCalendar *self = CLAWT_ROUTINE_CALENDAR(object);

	if (self->task_notify != NULL)
		self->task_notify(self->task_data);

	if (self->agent_notify != NULL)
		self->agent_notify(self->agent_data);

	g_clear_object(&self->config);
	g_clear_object(&self->runner);

	G_OBJECT_CLASS(clawt_routine_calendar_parent_class)->finalize(object);
}

static void
clawt_routine_calendar_class_init(
	ClawtRoutineCalendarClass *klass
){
	G_OBJECT_CLASS(klass)->finalize = clawt_routine_calendar_finalize;
}

static void
clawt_routine_calendar_init(
	ClawtRoutineCalendar *self
){
	(void)self;
}

ClawtRoutineCalendar *
clawt_routine_calendar_new(
	ClawtConfig        *config,
	ClawtRoutineRunner *runner
){
	ClawtRoutineCalendar *self;

	g_return_val_if_fail(CLAWT_IS_CONFIG(config), NULL);
	g_return_val_if_fail(runner == NULL || CLAWT_IS_ROUTINE_RUNNER(runner),
	                     NULL);

	self = g_object_new(CLAWT_TYPE_ROUTINE_CALENDAR, NULL);
	self->config = g_object_ref(config);
	self->runner = (runner != NULL) ? g_object_ref(runner) : NULL;

	return self;
}

void
clawt_routine_calendar_set_task_func(
	ClawtRoutineCalendar  *self,
	ClawtCalendarTaskFunc  func,
	gpointer               user_data,
	GDestroyNotify         notify
){
	g_return_if_fail(CLAWT_IS_ROUTINE_CALENDAR(self));

	if (self->task_notify != NULL)
		self->task_notify(self->task_data);

	self->task_func = func;
	self->task_data = user_data;
	self->task_notify = notify;
}

void
clawt_routine_calendar_set_agent_func(
	ClawtRoutineCalendar   *self,
	ClawtCalendarAgentFunc  func,
	gpointer                user_data,
	GDestroyNotify          notify
){
	g_return_if_fail(CLAWT_IS_ROUTINE_CALENDAR(self));

	if (self->agent_notify != NULL)
		self->agent_notify(self->agent_data);

	self->agent_func = func;
	self->agent_data = user_data;
	self->agent_notify = notify;
}

void
clawt_routine_calendar_set_limits(
	ClawtRoutineCalendar *self,
	guint                 max_days,
	guint                 max_occurrences
){
	g_return_if_fail(CLAWT_IS_ROUTINE_CALENDAR(self));

	self->max_days = max_days;
	self->max_occurrences = max_occurrences;
}

/* An override, else the config, else the default -- never 0. */
static guint
limit_of(
	ClawtRoutineCalendar *self,
	guint                 override,
	const gchar          *key,
	guint                 fallback
){
	gint64 configured;

	if (override > 0)
		return override;

	configured = clawt_config_get_int(self->config, key);

	return (configured > 0) ? (guint)MIN(configured, (gint64)G_MAXUINT)
	                        : fallback;
}

/* ── Who is in the picture ───────────────────────────────────────── */

/* Whether @agent_id passes the agent and team filters. */
static gboolean
agent_matches(
	ClawtRoutineCalendar *self,
	const gchar          *agent_id,
	const gchar          *agent,
	const gchar          *team
){
	ClawtAgentConfig *config;

	if (agent != NULL && *agent != '\0' && g_strcmp0(agent, agent_id) != 0)
		return FALSE;

	if (team == NULL || *team == '\0')
		return TRUE;

	config = (agent_id != NULL)
		? clawt_config_get_agent(self->config, agent_id) : NULL;

	return config != NULL &&
	       g_strcmp0(clawt_agent_config_get_string(config, "team"), team) == 0;
}

/* What a card says: the routine's description, or its id. */
static const gchar *
routine_title(
	ClawtRoutine *routine
){
	const gchar *description = clawt_routine_get_string(routine,
	                                                    "description");

	return (description != NULL && *description != '\0')
		? description : clawt_routine_get_id(routine);
}

/*
 * The shortest gap between a schedule's runs, as a hint for folding:
 * an hourly routine says 3600.  Looked at from the start of the window
 * over a handful of runs, which is enough to tell hourly from daily and
 * cheap enough to do for every routine.
 */
static gint64
repeat_every(
	ClawtCron *cron,
	GDateTime *from
){
	gint64 gap = clawt_cron_shortest_gap(cron, from, 8);

	return (gap == G_MAXINT64) ? 0 : gap;
}

/* ── Turning the record into a status ────────────────────────────── */

/*
 * A started run's status, from its task.  Without a task function, or
 * for a task the daemon no longer knows, a run that started is `ok`:
 * starting is what the runner can vouch for.
 */
static void
status_from_task(
	ClawtRoutineCalendar *self,
	Occurrence           *occurrence
){
	ClawtTaskState state = CLAWT_TASK_PENDING;
	gchar *room = NULL;
	gchar *reason = NULL;

	occurrence->status = CLAWT_OCCURRENCE_OK;

	if (self->task_func == NULL || occurrence->task == NULL ||
	    !self->task_func(occurrence->task, &state, &room, &reason,
	                     self->task_data)) {
		g_free(room);
		g_free(reason);
		return;
	}

	occurrence->task_state = (gint)state;
	g_free(occurrence->room);
	occurrence->room = room;

	switch (state) {
	case CLAWT_TASK_COMPLETED:
		occurrence->status = CLAWT_OCCURRENCE_OK;
		break;

	case CLAWT_TASK_FAILED:
	case CLAWT_TASK_STALLED:
		occurrence->status = CLAWT_OCCURRENCE_FAILED;
		break;

	case CLAWT_TASK_CANCELLED:
		occurrence->status = CLAWT_OCCURRENCE_SKIPPED;
		occurrence->reason = "cancelled";
		break;

	case CLAWT_TASK_PENDING:
	case CLAWT_TASK_RUNNING:
		/*
		 * Waiting in the mailbox of an agent that is not up: it has not
		 * run, and will when somebody starts the agent.  Skipped, and
		 * marked queued so a client can say it is not lost.
		 *
		 * Running as well as pending, because the daemon marks a
		 * routine's task running *before* it delivers the prompt (so an
		 * agent that answers at once does not complete a task that never
		 * started) -- a run held by a stopped agent reads as running.
		 * One whose agent died mid-turn is failed by the daemon, so an
		 * open task on an agent that is down is one still waiting.
		 */
		if (self->agent_func != NULL && occurrence->agent != NULL &&
		    !self->agent_func(occurrence->agent, self->agent_data)) {
			occurrence->status = CLAWT_OCCURRENCE_SKIPPED;
			occurrence->reason = "agent_stopped";
			occurrence->queued = TRUE;
			break;
		}
		occurrence->status = CLAWT_OCCURRENCE_RUNNING;
		break;

	case CLAWT_TASK_UNASSIGNED:
	case CLAWT_TASK_BLOCKED:
	case CLAWT_TASK_REVIEW:
	default:
		occurrence->status = CLAWT_OCCURRENCE_RUNNING;
		break;
	}

	/* The task's own reason is the better detail when it has one. */
	if (reason != NULL && *reason != '\0' &&
	    occurrence->status != CLAWT_OCCURRENCE_OK &&
	    occurrence->status != CLAWT_OCCURRENCE_RUNNING) {
		g_free(occurrence->detail);
		occurrence->detail = reason;
		reason = NULL;
	}

	g_free(reason);
}

/* Fills an occurrence in from what the runner recorded for it. */
static void
apply_entry(
	ClawtRoutineCalendar *self,
	Occurrence           *occurrence,
	const ClawtRunEntry  *entry
){
	occurrence->by_hand = entry->by_hand;
	occurrence->started_at = entry->at;
	g_free(occurrence->detail);
	occurrence->detail = g_strdup(entry->detail);
	g_free(occurrence->task);
	occurrence->task = g_strdup(entry->task_id);

	switch (entry->outcome) {
	case CLAWT_RUN_OUTCOME_STARTED:
		status_from_task(self, occurrence);
		break;

	case CLAWT_RUN_OUTCOME_FAILED:
		occurrence->status = CLAWT_OCCURRENCE_FAILED;
		break;

	case CLAWT_RUN_OUTCOME_REFUSED:
		occurrence->status = CLAWT_OCCURRENCE_SKIPPED;
		occurrence->reason = "refused";
		break;

	case CLAWT_RUN_OUTCOME_MISSED:
		occurrence->status = CLAWT_OCCURRENCE_SKIPPED;
		occurrence->reason = "daemon_down";
		occurrence->started_at = 0;
		break;

	case CLAWT_RUN_OUTCOME_CAUGHT_UP:
		occurrence->status = CLAWT_OCCURRENCE_SKIPPED;
		occurrence->reason = "catch_up";
		occurrence->started_at = 0;
		break;

	default:
		occurrence->status = CLAWT_OCCURRENCE_UNKNOWN;
		break;
	}
}

/* ── Expanding ───────────────────────────────────────────────────── */

typedef struct {
	GPtrArray   *occurrences;   /* Occurrence*, owned */
	JsonBuilder *routines;      /* inside an array */
	JsonBuilder *unscheduled;   /* inside an array */
	guint        max;           /* the occurrence limit */
	guint        total;
	guint        busiest;       /* the most one routine contributed */
	gchar       *busiest_id;
	gboolean     over;          /* some routine alone passed the limit */
} Gather;

/* Counts one routine's contribution towards the limit. */
static void
gather_count(
	Gather      *gather,
	const gchar *id,
	guint        count,
	gboolean     truncated
){
	gather->total += count;

	if (truncated)
		gather->over = TRUE;

	if (count > gather->busiest || gather->busiest_id == NULL) {
		gather->busiest = count;
		g_free(gather->busiest_id);
		gather->busiest_id = g_strdup(id);
	}
}

/* One routine's entry in the reply's `routines` index. */
static void
add_routine_index(
	JsonBuilder  *builder,
	const gchar  *source,
	const gchar  *id,
	const gchar  *agent,
	const gchar  *title,
	gboolean      enabled,
	const gchar  *schedule,
	const gchar  *expression,
	guint         jitter,
	gint64        every,
	gint64        interval,
	const gchar  *problem
){
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "source");
	json_builder_add_string_value(builder, source);
	json_builder_set_member_name(builder, "id");
	json_builder_add_string_value(builder, id);
	json_builder_set_member_name(builder, "agent");
	json_builder_add_string_value(builder, agent != NULL ? agent : "");
	json_builder_set_member_name(builder, "title");
	json_builder_add_string_value(builder, title);
	json_builder_set_member_name(builder, "enabled");
	json_builder_add_boolean_value(builder, enabled);

	if (schedule != NULL) {
		json_builder_set_member_name(builder, "schedule");
		json_builder_add_string_value(builder, schedule);
	}

	if (expression != NULL) {
		json_builder_set_member_name(builder, "expression");
		json_builder_add_string_value(builder, expression);
	}

	json_builder_set_member_name(builder, "jitter_seconds");
	json_builder_add_int_value(builder, (gint64)jitter);

	if (every > 0) {
		json_builder_set_member_name(builder, "every_seconds");
		json_builder_add_int_value(builder, every);
	}

	if (interval > 0) {
		json_builder_set_member_name(builder, "interval_seconds");
		json_builder_add_int_value(builder, interval);
	}

	if (problem != NULL) {
		json_builder_set_member_name(builder, "problem");
		json_builder_add_string_value(builder, problem);
	}

	json_builder_end_object(builder);
}

/*
 * One clawtilla routine's occurrences in the window: its schedule
 * expanded, joined with its history, plus every run in the history that
 * the schedule as it now stands would not have produced -- a Run now, or
 * a time from before the schedule was edited.  Those happened, and a
 * calendar that hid them would be wrong about the past.
 */
static void
gather_routine(
	ClawtRoutineCalendar *self,
	Gather               *gather,
	ClawtRoutine         *routine,
	GDateTime            *from,
	GDateTime            *to,
	GDateTime            *now
){
	const gchar *id = clawt_routine_get_id(routine);
	const gchar *agent = clawt_routine_get_string(routine, "agent");
	const gchar *schedule = clawt_routine_get_string(routine, "schedule");
	const gchar *title = routine_title(routine);
	guint jitter = clawt_routine_jitter_seconds(routine);
	gint64 now_s = g_date_time_to_unix(now);
	gint64 pending = 0;
	g_autofree gchar *expression = NULL;
	g_autoptr(GError) cron_error = NULL;
	g_autoptr(ClawtCron) cron = NULL;
	g_autoptr(GPtrArray) slots = NULL;
	g_autoptr(GPtrArray) history = NULL;
	g_autoptr(GHashTable) by_slot = NULL;
	g_autoptr(GHashTable) used = NULL;
	gboolean truncated = FALSE;
	guint count = 0;
	guint i;

	if (schedule == NULL)
		schedule = "manual";

	expression = clawt_routine_get_cron(routine, &cron_error);

	if (expression != NULL)
		cron = clawt_cron_parse(expression, &cron_error);

	add_routine_index(gather->routines, CLAWT_CALENDAR_SOURCE_ROUTINE, id,
	                  agent, title,
	                  clawt_routine_get_boolean(routine, "enabled"), schedule,
	                  expression, jitter,
	                  (cron != NULL) ? repeat_every(cron, from) : 0, 0,
	                  (cron_error != NULL) ? cron_error->message : NULL);

	/* What the runner remembers, keyed by the time it was for. */
	by_slot = g_hash_table_new(g_int64_hash, g_int64_equal);
	used = g_hash_table_new(g_direct_hash, g_direct_equal);

	if (self->runner != NULL) {
		history = clawt_routine_runner_get_history(self->runner, id,
		                                           g_date_time_to_unix(from),
		                                           g_date_time_to_unix(to));
		pending = clawt_routine_runner_get_pending_slot(self->runner, id);

		/* The last entry for a time wins: it is the latest word on it. */
		for (i = 0; i < history->len; i++) {
			ClawtRunEntry *entry = g_ptr_array_index(history, i);

			if (entry->slot > 0)
				g_hash_table_replace(by_slot, &entry->slot, entry);
		}
	}

	if (cron != NULL) {
		slots = clawt_cron_between(cron, from, to, gather->max + 1,
		                           &truncated);

		for (i = 0; i < slots->len; i++) {
			GDateTime *slot = g_ptr_array_index(slots, i);
			gint64 at = g_date_time_to_unix(slot);
			Occurrence *occurrence = occurrence_new(
				CLAWT_CALENDAR_SOURCE_ROUTINE, id, agent, title, schedule, at,
				jitter);
			ClawtRunEntry *entry = g_hash_table_lookup(by_slot, &at);

			if (entry != NULL) {
				apply_entry(self, occurrence, entry);
				g_hash_table_add(used, entry);
			} else if (at > now_s) {
				occurrence->status = CLAWT_OCCURRENCE_UPCOMING;
			} else if (pending == at) {
				occurrence->status = CLAWT_OCCURRENCE_DUE;
				occurrence->reason = "jitter";
			} else if (now_s - at < DUE_GRACE_SECONDS) {
				occurrence->status = CLAWT_OCCURRENCE_DUE;
				occurrence->reason = "next_tick";
			} else {
				occurrence->status = CLAWT_OCCURRENCE_UNKNOWN;
				occurrence->reason = "no_record";
			}

			g_ptr_array_add(gather->occurrences, occurrence);
			count++;
		}
	}

	/* The runs the schedule as it stands does not account for. */
	for (i = 0; history != NULL && i < history->len; i++) {
		ClawtRunEntry *entry = g_ptr_array_index(history, i);
		Occurrence *occurrence;

		if (g_hash_table_contains(used, entry))
			continue;

		/* An earlier entry for a time a later one already answered. */
		if (entry->slot > 0 &&
		    g_hash_table_lookup(by_slot, &entry->slot) != entry)
			continue;

		occurrence = occurrence_new(CLAWT_CALENDAR_SOURCE_ROUTINE, id, agent,
		                            title, schedule,
		                            (entry->slot > 0) ? entry->slot : entry->at,
		                            (entry->slot > 0) ? jitter : 0);
		occurrence->manual = (entry->slot == 0);
		apply_entry(self, occurrence, entry);
		g_ptr_array_add(gather->occurrences, occurrence);
		count++;
	}

	gather_count(gather, id, count, truncated);
}

/* One entry of an agent's libreclaw `cron:` list, as far as it matters here. */
typedef struct {
	gchar    *name;
	gchar    *schedule;    /* nullable */
	gint64    interval;    /* interval_seconds, 0 for none */
	gboolean  enabled;
} LibreclawJob;

static void
libreclaw_job_free(
	LibreclawJob *self
){
	if (self == NULL)
		return;

	g_free(self->name);
	g_free(self->schedule);
	g_free(self);
}

/*
 * The `cron:` list in an agent's `libreclaw:` passthrough.
 *
 * Read from the config rather than asked of the agent, so it is there
 * for a stopped agent too -- and so it is only what the file says: a job
 * somebody added at runtime through libreclaw's own command is not in
 * it.  `enabled` is read as libreclaw reads it, defaulting to on.
 *
 * Returns: (transfer full) (element-type LibreclawJob): the jobs
 */
static GPtrArray *
libreclaw_jobs(
	ClawtAgentConfig *agent_config
){
	GPtrArray *out = g_ptr_array_new_with_free_func(
		(GDestroyNotify)libreclaw_job_free);
	g_autofree gchar *raw = NULL;
	g_autoptr(YamlParser) parser = NULL;
	YamlNode *root;
	YamlSequence *jobs;
	guint n;
	guint i;

	raw = clawt_agent_config_get_raw_yaml(agent_config, "libreclaw");

	if (raw == NULL || *raw == '\0')
		return out;

	parser = yaml_parser_new();

	if (!yaml_parser_load_from_data(parser, raw, -1, NULL))
		return out;

	root = yaml_parser_get_root(parser);

	if (root == NULL || yaml_node_get_node_type(root) != YAML_NODE_MAPPING)
		return out;

	jobs = yaml_mapping_get_sequence_member(yaml_node_get_mapping(root),
	                                        "cron");
	n = (jobs != NULL) ? yaml_sequence_get_length(jobs) : 0;

	for (i = 0; i < n; i++) {
		YamlNode *node = yaml_sequence_get_element(jobs, i);
		YamlMapping *job;
		LibreclawJob *entry;
		const gchar *name;
		const gchar *enabled_text;

		if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
			continue;

		job = yaml_node_get_mapping(node);
		name = yaml_mapping_get_string_member(job, "name");

		if (name == NULL)
			continue;

		entry = g_new0(LibreclawJob, 1);
		entry->name = g_strdup(name);
		entry->schedule = g_strdup(yaml_mapping_get_string_member(job,
		                                                          "schedule"));
		entry->interval = yaml_mapping_get_int_member(job, "interval_seconds");
		entry->enabled = TRUE;

		enabled_text = yaml_mapping_get_string_member(job, "enabled");
		if (enabled_text != NULL)
			entry->enabled = g_ascii_strcasecmp(enabled_text, "true") == 0 ||
			                 g_ascii_strcasecmp(enabled_text, "yes") == 0 ||
			                 g_strcmp0(enabled_text, "1") == 0;

		g_ptr_array_add(out, entry);
	}

	return out;
}

/*
 * An agent's libreclaw crons in the window.  A schedule clawt_cron
 * cannot parse, and an interval job -- which counts from whenever the
 * agent's process started, so has no place on a calendar -- goes in
 * `unscheduled` with what is known about it.
 */
static void
gather_libreclaw(
	ClawtRoutineCalendar *self,
	Gather               *gather,
	ClawtAgentConfig     *agent_config,
	GDateTime            *from,
	GDateTime            *to,
	GDateTime            *now,
	gboolean              include_disabled
){
	const gchar *agent = clawt_agent_config_get_id(agent_config);
	gboolean agent_on = clawt_agent_config_get_boolean(agent_config,
	                                                   "enabled");
	g_autoptr(GPtrArray) jobs = libreclaw_jobs(agent_config);
	gint64 now_s = g_date_time_to_unix(now);
	guint i;

	for (i = 0; i < jobs->len; i++) {
		LibreclawJob *job = g_ptr_array_index(jobs, i);
		gboolean on = job->enabled && agent_on;
		g_autoptr(ClawtCron) cron = NULL;
		g_autoptr(GError) cron_error = NULL;
		g_autoptr(GPtrArray) slots = NULL;
		gboolean truncated = FALSE;
		guint k;

		if (!include_disabled && !on)
			continue;

		if (job->schedule != NULL)
			cron = clawt_cron_parse(job->schedule, &cron_error);

		if (cron == NULL) {
			add_routine_index(gather->unscheduled,
			                  CLAWT_CALENDAR_SOURCE_LIBRECLAW_CRON, job->name,
			                  agent, job->name, on, NULL, job->schedule, 0, 0,
			                  (job->schedule == NULL) ? job->interval : 0,
			                  (cron_error != NULL) ? cron_error->message
			                  : (job->schedule == NULL && job->interval <= 0)
			                      ? "neither schedule nor interval_seconds"
			                      : NULL);
			continue;
		}

		add_routine_index(gather->routines,
		                  CLAWT_CALENDAR_SOURCE_LIBRECLAW_CRON, job->name, agent,
		                  job->name, on, "custom", job->schedule, 0,
		                  repeat_every(cron, from), 0, NULL);

		slots = clawt_cron_between(cron, from, to, gather->max + 1, &truncated);

		for (k = 0; k < slots->len; k++) {
			gint64 at = g_date_time_to_unix(g_ptr_array_index(slots, k));
			Occurrence *occurrence = occurrence_new(
				CLAWT_CALENDAR_SOURCE_LIBRECLAW_CRON, job->name, agent,
				job->name, "custom", at, 0);

			/* clawtilla does not run these, so it cannot say how one went. */
			if (at > now_s) {
				occurrence->status = CLAWT_OCCURRENCE_UPCOMING;
			} else {
				occurrence->status = CLAWT_OCCURRENCE_UNKNOWN;
				occurrence->reason = "not_tracked";
			}

			g_ptr_array_add(gather->occurrences, occurrence);
		}

		gather_count(gather, job->name, slots->len, truncated);
	}
}

/*
 * The next @limit runs of one schedule from @first on, as upcoming
 * occurrences.  @first is consumed.
 */
static void
add_upcoming(
	GPtrArray   *runs,
	const gchar *source,
	const gchar *id,
	const gchar *agent,
	const gchar *title,
	const gchar *repeat,
	ClawtCron   *cron,
	GDateTime   *first,
	guint        jitter,
	guint        limit
){
	GDateTime *next = first;
	guint k;

	for (k = 0; next != NULL && k < limit; k++) {
		Occurrence *occurrence = occurrence_new(source, id, agent, title,
		                                        repeat,
		                                        g_date_time_to_unix(next),
		                                        jitter);
		GDateTime *after = next;

		occurrence->status = CLAWT_OCCURRENCE_UPCOMING;
		g_ptr_array_add(runs, occurrence);

		next = clawt_cron_next(cron, after);
		g_date_time_unref(after);
	}

	if (next != NULL)
		g_date_time_unref(next);
}

/* ── Writing it out ──────────────────────────────────────────────── */

/* A Unix time as the wire's UTC: `2026-11-24T14:00:00Z`. */
static gchar *
utc_text(
	gint64 when
){
	g_autoptr(GDateTime) utc = g_date_time_new_from_unix_utc(when);

	return (utc != NULL) ? g_date_time_format(utc, "%Y-%m-%dT%H:%M:%SZ")
	                     : g_strdup("");
}

static void
add_time_member(
	JsonBuilder *builder,
	const gchar *name,
	gint64       when
){
	g_autofree gchar *text = utc_text(when);

	json_builder_set_member_name(builder, name);
	json_builder_add_string_value(builder, text);
}

static void
add_occurrence(
	JsonBuilder      *builder,
	const Occurrence *occurrence
){
	json_builder_begin_object(builder);

	json_builder_set_member_name(builder, "source");
	json_builder_add_string_value(builder, occurrence->source);
	json_builder_set_member_name(builder, "routine");
	json_builder_add_string_value(builder, occurrence->routine);
	json_builder_set_member_name(builder, "agent");
	json_builder_add_string_value(builder, occurrence->agent != NULL
	                                       ? occurrence->agent : "");
	json_builder_set_member_name(builder, "title");
	json_builder_add_string_value(builder, occurrence->title);
	add_time_member(builder, "at", occurrence->at);
	add_time_member(builder, "earliest", occurrence->at);
	add_time_member(builder, "latest", occurrence->latest);
	json_builder_set_member_name(builder, "kind");
	json_builder_add_string_value(builder, occurrence->manual ? "manual"
	                                                          : "scheduled");
	json_builder_set_member_name(builder, "repeat");
	json_builder_add_string_value(builder, occurrence->repeat != NULL
	                                       ? occurrence->repeat : "");
	json_builder_set_member_name(builder, "status");
	json_builder_add_string_value(builder,
		clawt_enum_to_nick(CLAWT_TYPE_OCCURRENCE_STATUS,
		                   (gint)occurrence->status));

	if (occurrence->reason != NULL) {
		json_builder_set_member_name(builder, "reason");
		json_builder_add_string_value(builder, occurrence->reason);
	}

	if (occurrence->detail != NULL) {
		json_builder_set_member_name(builder, "detail");
		json_builder_add_string_value(builder, occurrence->detail);
	}

	if (occurrence->started_at > 0)
		add_time_member(builder, "started_at", occurrence->started_at);

	if (occurrence->by_hand) {
		json_builder_set_member_name(builder, "by_hand");
		json_builder_add_boolean_value(builder, TRUE);
	}

	if (occurrence->queued) {
		json_builder_set_member_name(builder, "queued");
		json_builder_add_boolean_value(builder, TRUE);
	}

	if (occurrence->task != NULL) {
		json_builder_set_member_name(builder, "task");
		json_builder_add_string_value(builder, occurrence->task);
	}

	if (occurrence->task_state >= 0) {
		json_builder_set_member_name(builder, "task_state");
		json_builder_add_string_value(builder,
			clawt_enum_to_nick(CLAWT_TYPE_TASK_STATE, occurrence->task_state));
	}

	if (occurrence->room != NULL) {
		json_builder_set_member_name(builder, "room");
		json_builder_add_string_value(builder, occurrence->room);
	}

	json_builder_end_object(builder);
}

/*
 * How often a group's runs come, in words: "every hour", "every 15
 * minutes", or -- when the gaps are not all the same -- "12 runs".
 * Then the span in local time, "08:00-18:00".
 */
static gchar *
group_label(
	GPtrArray *members,
	gint64    *out_every
){
	Occurrence *first = g_ptr_array_index(members, 0);
	Occurrence *last = g_ptr_array_index(members, members->len - 1);
	g_autoptr(GDateTime) first_local = NULL;
	g_autoptr(GDateTime) last_local = NULL;
	g_autofree gchar *how = NULL;
	g_autofree gchar *from_text = NULL;
	g_autofree gchar *to_text = NULL;
	gint64 every;
	guint i;

	every = ((Occurrence *)g_ptr_array_index(members, 1))->at - first->at;

	for (i = 2; i < members->len && every > 0; i++) {
		Occurrence *before = g_ptr_array_index(members, i - 1);
		Occurrence *here = g_ptr_array_index(members, i);

		if (here->at - before->at != every)
			every = 0;
	}

	*out_every = every;

	if (every == 3600)
		how = g_strdup("every hour");
	else if (every > 3600 && every % 3600 == 0)
		how = g_strdup_printf("every %" G_GINT64_FORMAT " hours",
		                      every / 3600);
	else if (every == 60)
		how = g_strdup("every minute");
	else if (every > 60 && every % 60 == 0)
		how = g_strdup_printf("every %" G_GINT64_FORMAT " minutes",
		                      every / 60);
	else
		how = g_strdup_printf("%u runs", members->len);

	first_local = g_date_time_new_from_unix_local(first->at);
	last_local = g_date_time_new_from_unix_local(last->at);
	from_text = g_date_time_format(first_local, "%H:%M");
	to_text = g_date_time_format(last_local, "%H:%M");

	return g_strdup_printf("%s, %s-%s", how, from_text, to_text);
}

/*
 * The status a folded card shows: the worst thing in it, so a failed
 * run is not hidden behind ten good ones.
 */
static ClawtOccurrenceStatus
group_status(
	GPtrArray *members
){
	static const ClawtOccurrenceStatus order[] = {
		CLAWT_OCCURRENCE_FAILED,
		CLAWT_OCCURRENCE_RUNNING,
		CLAWT_OCCURRENCE_DUE,
		CLAWT_OCCURRENCE_SKIPPED,
		CLAWT_OCCURRENCE_UNKNOWN,
		CLAWT_OCCURRENCE_OK,
		CLAWT_OCCURRENCE_UPCOMING
	};
	gsize k;
	guint i;

	for (k = 0; k < G_N_ELEMENTS(order); k++)
		for (i = 0; i < members->len; i++)
			if (((Occurrence *)g_ptr_array_index(members, i))->status ==
			    order[k])
				return order[k];

	return CLAWT_OCCURRENCE_UPCOMING;
}

static void
add_group(
	JsonBuilder *builder,
	GPtrArray   *members,
	const gchar *day
){
	Occurrence *first = g_ptr_array_index(members, 0);
	Occurrence *last = g_ptr_array_index(members, members->len - 1);
	g_autofree gchar *label = NULL;
	guint counts[CLAWT_OCCURRENCE_UNKNOWN + 1] = { 0 };
	gint64 every = 0;
	guint i;

	label = group_label(members, &every);

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "source");
	json_builder_add_string_value(builder, first->source);
	json_builder_set_member_name(builder, "routine");
	json_builder_add_string_value(builder, first->routine);
	json_builder_set_member_name(builder, "agent");
	json_builder_add_string_value(builder, first->agent != NULL
	                                       ? first->agent : "");
	json_builder_set_member_name(builder, "title");
	json_builder_add_string_value(builder, first->title);
	json_builder_set_member_name(builder, "day");
	json_builder_add_string_value(builder, day);
	add_time_member(builder, "first", first->at);
	add_time_member(builder, "last", last->at);
	json_builder_set_member_name(builder, "count");
	json_builder_add_int_value(builder, (gint64)members->len);

	if (every > 0) {
		json_builder_set_member_name(builder, "every_seconds");
		json_builder_add_int_value(builder, every);
	}

	json_builder_set_member_name(builder, "label");
	json_builder_add_string_value(builder, label);
	json_builder_set_member_name(builder, "status");
	json_builder_add_string_value(builder,
		clawt_enum_to_nick(CLAWT_TYPE_OCCURRENCE_STATUS,
		                   (gint)group_status(members)));

	for (i = 0; i < members->len; i++)
		counts[((Occurrence *)g_ptr_array_index(members, i))->status]++;

	json_builder_set_member_name(builder, "statuses");
	json_builder_begin_object(builder);
	for (i = 0; i < G_N_ELEMENTS(counts); i++) {
		if (counts[i] == 0)
			continue;
		json_builder_set_member_name(builder,
			clawt_enum_to_nick(CLAWT_TYPE_OCCURRENCE_STATUS, (gint)i));
		json_builder_add_int_value(builder, (gint64)counts[i]);
	}
	json_builder_end_object(builder);

	json_builder_set_member_name(builder, "occurrences");
	json_builder_begin_array(builder);
	for (i = 0; i < members->len; i++)
		add_occurrence(builder, g_ptr_array_index(members, i));
	json_builder_end_array(builder);

	json_builder_end_object(builder);
}

/* The local calendar date of a Unix time: `2026-11-24`. */
static gchar *
local_day(
	gint64 when
){
	g_autoptr(GDateTime) local = g_date_time_new_from_unix_local(when);

	return g_date_time_format(local, "%Y-%m-%d");
}

/*
 * Writes the occurrences out, folding each routine's runs on one local
 * day into a group when asked and there are enough of them.  Groups are
 * in the order of their first run, like everything else.
 */
static void
add_occurrences(
	JsonBuilder *builder,
	GPtrArray   *occurrences,
	gboolean     collapse
){
	g_autoptr(GHashTable) groups = NULL;   /* key -> GPtrArray of Occurrence* */
	g_autoptr(GPtrArray) order = NULL;     /* keys, in first-run order */
	guint i;

	if (!collapse) {
		json_builder_set_member_name(builder, "occurrences");
		json_builder_begin_array(builder);
		for (i = 0; i < occurrences->len; i++)
			add_occurrence(builder, g_ptr_array_index(occurrences, i));
		json_builder_end_array(builder);
		return;
	}

	groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                               (GDestroyNotify)g_ptr_array_unref);
	order = g_ptr_array_new();

	for (i = 0; i < occurrences->len; i++) {
		Occurrence *occurrence = g_ptr_array_index(occurrences, i);
		g_autofree gchar *day = local_day(occurrence->at);
		gchar *key = g_strdup_printf("%s\x1f%s\x1f%s\x1f%s",
		                             occurrence->source, occurrence->agent,
		                             occurrence->routine, day);
		GPtrArray *members = g_hash_table_lookup(groups, key);

		if (members == NULL) {
			members = g_ptr_array_new();
			g_hash_table_insert(groups, key, members);
			g_ptr_array_add(order, key);
		} else {
			g_free(key);
		}

		g_ptr_array_add(members, occurrence);
	}

	json_builder_set_member_name(builder, "occurrences");
	json_builder_begin_array(builder);
	for (i = 0; i < occurrences->len; i++) {
		Occurrence *occurrence = g_ptr_array_index(occurrences, i);
		g_autofree gchar *day = local_day(occurrence->at);
		g_autofree gchar *key = g_strdup_printf(
			"%s\x1f%s\x1f%s\x1f%s", occurrence->source, occurrence->agent,
			occurrence->routine, day);
		GPtrArray *members = g_hash_table_lookup(groups, key);

		if (members->len < CLAWT_CALENDAR_COLLAPSE_MIN)
			add_occurrence(builder, occurrence);
	}
	json_builder_end_array(builder);

	json_builder_set_member_name(builder, "groups");
	json_builder_begin_array(builder);
	for (i = 0; i < order->len; i++) {
		const gchar *key = g_ptr_array_index(order, i);
		GPtrArray *members = g_hash_table_lookup(groups, key);
		g_autofree gchar *day = NULL;

		if (members->len < CLAWT_CALENDAR_COLLAPSE_MIN)
			continue;

		day = local_day(((Occurrence *)g_ptr_array_index(members, 0))->at);
		add_group(builder, members, day);
	}
	json_builder_end_array(builder);
}

/* ── The two replies ─────────────────────────────────────────────── */

JsonNode *
clawt_routine_calendar_occurrences(
	ClawtRoutineCalendar  *self,
	GDateTime             *from,
	GDateTime             *to,
	GDateTime             *now,
	const gchar           *agent,
	const gchar           *team,
	gboolean               include_disabled,
	gboolean               collapse,
	GError               **error
){
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonBuilder) routines = NULL;
	g_autoptr(JsonBuilder) unscheduled = NULL;
	g_autoptr(GPtrArray) occurrences = NULL;
	g_autofree gchar *zone = NULL;
	Gather gather = { 0 };
	GPtrArray *list;
	guint max_days;
	gint64 span;
	guint i;

	g_return_val_if_fail(CLAWT_IS_ROUTINE_CALENDAR(self), NULL);
	g_return_val_if_fail(from != NULL && to != NULL && now != NULL, NULL);

	/* ── the window ── */
	span = g_date_time_difference(to, from) / G_USEC_PER_SEC;
	max_days = limit_of(self, self->max_days, "daemon.calendar_max_days",
	                    DEFAULT_MAX_DAYS);

	if (span <= 0) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		                    "the window is empty: 'to' must be after 'from'");
		return NULL;
	}

	if (span > (gint64)max_days * 86400) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "the window is %" G_GINT64_FORMAT " days and at most %u "
		            "may be asked for at once (daemon.calendar_max_days)",
		            (span + 86399) / 86400, max_days);
		return NULL;
	}

	occurrences = g_ptr_array_new_with_free_func(
		(GDestroyNotify)occurrence_free);
	routines = json_builder_new();
	unscheduled = json_builder_new();
	json_builder_begin_array(routines);
	json_builder_begin_array(unscheduled);

	gather.occurrences = occurrences;
	gather.routines = routines;
	gather.unscheduled = unscheduled;
	gather.max = limit_of(self, self->max_occurrences,
	                      "daemon.calendar_max_occurrences",
	                      DEFAULT_MAX_OCCURRENCES);

	/* ── clawtilla's own routines ── */
	list = clawt_config_get_routines(self->config);

	for (i = 0; list != NULL && i < list->len; i++) {
		ClawtRoutine *routine = g_ptr_array_index(list, i);

		if (!include_disabled && !clawt_routine_get_boolean(routine, "enabled"))
			continue;

		if (!agent_matches(self, clawt_routine_get_string(routine, "agent"),
		                   agent, team))
			continue;

		gather_routine(self, &gather, routine, from, to, now);

		if (gather.over || gather.total > gather.max)
			break;
	}

	/* ── libreclaw crons, per agent ── */
	list = clawt_config_get_agents(self->config);

	for (i = 0; !gather.over && gather.total <= gather.max &&
	            list != NULL && i < list->len; i++) {
		ClawtAgentConfig *agent_config = g_ptr_array_index(list, i);

		if (!agent_matches(self, clawt_agent_config_get_id(agent_config),
		                   agent, team))
			continue;

		gather_libreclaw(self, &gather, agent_config, from, to, now,
		                 include_disabled);
	}

	json_builder_end_array(routines);
	json_builder_end_array(unscheduled);

	if (gather.over || gather.total > gather.max) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "the window holds more than %u occurrences "
		            "(daemon.calendar_max_occurrences); '%s' alone has %s%u -- "
		            "narrow the window or filter by agent or team",
		            gather.max, gather.busiest_id != NULL
		                        ? gather.busiest_id : "?",
		            gather.over ? "more than " : "",
		            gather.over ? gather.max : gather.busiest);
		g_free(gather.busiest_id);
		return NULL;
	}

	g_free(gather.busiest_id);
	g_ptr_array_sort(occurrences, occurrence_compare);
	zone = clawt_routine_calendar_zone_name();

	/* ── the reply ── */
	builder = json_builder_new();
	json_builder_begin_object(builder);
	add_time_member(builder, "from", g_date_time_to_unix(from));
	add_time_member(builder, "to", g_date_time_to_unix(to));
	add_time_member(builder, "now", g_date_time_to_unix(now));
	json_builder_set_member_name(builder, "time_zone");
	json_builder_add_string_value(builder, zone);
	json_builder_set_member_name(builder, "count");
	json_builder_add_int_value(builder, (gint64)occurrences->len);
	json_builder_set_member_name(builder, "routines");
	json_builder_add_value(builder, json_builder_get_root(routines));
	json_builder_set_member_name(builder, "unscheduled");
	json_builder_add_value(builder, json_builder_get_root(unscheduled));
	add_occurrences(builder, occurrences, collapse);
	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

JsonNode *
clawt_routine_calendar_upcoming(
	ClawtRoutineCalendar  *self,
	GDateTime             *now,
	guint                  limit,
	const gchar           *agent,
	const gchar           *team,
	GError               **error
){
	g_autoptr(GPtrArray) runs = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autofree gchar *zone = NULL;
	GPtrArray *list;
	guint i;

	g_return_val_if_fail(CLAWT_IS_ROUTINE_CALENDAR(self), NULL);
	g_return_val_if_fail(now != NULL, NULL);

	if (limit == 0)
		limit = DEFAULT_UPCOMING;

	if (limit > CLAWT_CALENDAR_UPCOMING_MAX) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "at most %d upcoming runs may be asked for at once",
		            CLAWT_CALENDAR_UPCOMING_MAX);
		return NULL;
	}

	runs = g_ptr_array_new_with_free_func((GDestroyNotify)occurrence_free);

	/*
	 * Each routine's next @limit runs, then the soonest @limit of all of
	 * them.  The first of each is the runner's own next_run_after(), the
	 * function routine.list's `next_run` comes from -- one function, so
	 * the panel and the list cannot disagree.
	 */
	list = clawt_config_get_routines(self->config);

	for (i = 0; list != NULL && i < list->len; i++) {
		ClawtRoutine *routine = g_ptr_array_index(list, i);
		const gchar *id = clawt_routine_get_id(routine);
		const gchar *schedule = clawt_routine_get_string(routine, "schedule");
		g_autofree gchar *expression = NULL;
		g_autoptr(ClawtCron) cron = NULL;
		g_autoptr(GDateTime) next = NULL;
		guint jitter = clawt_routine_jitter_seconds(routine);

		if (!clawt_routine_get_boolean(routine, "enabled") ||
		    !agent_matches(self, clawt_routine_get_string(routine, "agent"),
		                   agent, team))
			continue;

		expression = clawt_routine_get_cron(routine, NULL);
		cron = (expression != NULL) ? clawt_cron_parse(expression, NULL)
		                            : NULL;

		if (cron == NULL)
			continue;

		next = (self->runner != NULL)
			? clawt_routine_runner_next_run_after(self->runner, id, now)
			: clawt_cron_next(cron, now);

		add_upcoming(runs, CLAWT_CALENDAR_SOURCE_ROUTINE, id,
		             clawt_routine_get_string(routine, "agent"),
		             routine_title(routine),
		             (schedule != NULL) ? schedule : "manual", cron,
		             g_steal_pointer(&next), jitter, limit);
	}

	/* The libreclaw crons with a schedule, labelled by their source. */
	list = clawt_config_get_agents(self->config);

	for (i = 0; list != NULL && i < list->len; i++) {
		ClawtAgentConfig *agent_config = g_ptr_array_index(list, i);
		g_autoptr(GPtrArray) jobs = NULL;
		guint k;

		if (!clawt_agent_config_get_boolean(agent_config, "enabled") ||
		    !agent_matches(self, clawt_agent_config_get_id(agent_config),
		                   agent, team))
			continue;

		jobs = libreclaw_jobs(agent_config);

		for (k = 0; k < jobs->len; k++) {
			LibreclawJob *job = g_ptr_array_index(jobs, k);
			g_autoptr(ClawtCron) cron = NULL;

			if (!job->enabled || job->schedule == NULL)
				continue;

			cron = clawt_cron_parse(job->schedule, NULL);

			if (cron == NULL)
				continue;

			add_upcoming(runs, CLAWT_CALENDAR_SOURCE_LIBRECLAW_CRON, job->name,
			             clawt_agent_config_get_id(agent_config), job->name,
			             "custom", cron, clawt_cron_next(cron, now), 0, limit);
		}
	}

	g_ptr_array_sort(runs, occurrence_compare);

	if (runs->len > limit)
		g_ptr_array_remove_range(runs, limit, runs->len - limit);

	zone = clawt_routine_calendar_zone_name();

	builder = json_builder_new();
	json_builder_begin_object(builder);
	add_time_member(builder, "now", g_date_time_to_unix(now));
	json_builder_set_member_name(builder, "time_zone");
	json_builder_add_string_value(builder, zone);
	json_builder_set_member_name(builder, "runs");
	json_builder_begin_array(builder);
	for (i = 0; i < runs->len; i++)
		add_occurrence(builder, g_ptr_array_index(runs, i));
	json_builder_end_array(builder);
	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

/* ── The zone ────────────────────────────────────────────────────── */

gchar *
clawt_routine_calendar_zone_name(void)
{
	g_autoptr(GTimeZone) zone = g_time_zone_new_local();
	const gchar *identifier = g_time_zone_get_identifier(zone);
	g_autofree gchar *target = NULL;
	const gchar *within;

	/*
	 * A TZ such as "America/Detroit" is the name.  ":/etc/localtime",
	 * an absolute path, or GLib's own "Local" is not, and neither is
	 * "UTC" when it only means nothing was found -- so those fall
	 * through to the file /etc/localtime points at.
	 */
	if (identifier != NULL && *identifier != '\0' && *identifier != ':' &&
	    *identifier != '/' && g_strcmp0(identifier, "Local") != 0 &&
	    g_strcmp0(identifier, "UTC") != 0)
		return g_strdup(identifier);

	target = g_file_read_link("/etc/localtime", NULL);
	within = (target != NULL) ? strstr(target, "zoneinfo/") : NULL;

	if (within != NULL && within[strlen("zoneinfo/")] != '\0')
		return g_strdup(within + strlen("zoneinfo/"));

	return g_strdup("UTC");
}

/* ── iCalendar ───────────────────────────────────────────────────── */

/* TEXT escaping (RFC 5545 3.3.11): backslash, semicolon, comma, newline. */
static gchar *
ics_escape(
	const gchar *text
){
	GString *out = g_string_new(NULL);
	const gchar *p;

	for (p = (text != NULL) ? text : ""; *p != '\0'; p++) {
		switch (*p) {
		case '\\':
			g_string_append(out, "\\\\");
			break;
		case ';':
			g_string_append(out, "\\;");
			break;
		case ',':
			g_string_append(out, "\\,");
			break;
		case '\n':
			g_string_append(out, "\\n");
			break;
		case '\r':
			break;
		default:
			g_string_append_c(out, *p);
			break;
		}
	}

	return g_string_free(out, FALSE);
}

/*
 * One content line, folded (RFC 5545 3.1): at most 75 octets a physical
 * line, each continuation starting with a space that counts towards its
 * 75, and never a break inside a UTF-8 character -- a fold that split one
 * would hand the importer two halves of a letter.
 */
static void
ics_line(
	GString     *out,
	const gchar *line
){
	gsize length = strlen(line);
	gsize start = 0;
	gboolean first = TRUE;

	while (start < length) {
		gsize room = first ? ICS_LINE_OCTETS : ICS_LINE_OCTETS - 1;
		gsize end = MIN(start + room, length);

		/* Back up off a continuation byte (10xxxxxx). */
		while (end < length && end > start &&
		       ((guchar)line[end] & 0xC0) == 0x80)
			end--;

		if (!first)
			g_string_append_c(out, ' ');

		g_string_append_len(out, line + start, (gssize)(end - start));
		g_string_append(out, "\r\n");
		start = end;
		first = FALSE;
	}

	if (length == 0)
		g_string_append(out, "\r\n");
}

/* An ISO-8601 UTC time from the reply as iCalendar's 20261124T140000Z. */
static gchar *
ics_time(
	const gchar *iso
){
	g_autoptr(GDateTime) parsed = NULL;
	g_autoptr(GDateTime) utc = NULL;

	parsed = (iso != NULL) ? g_date_time_new_from_iso8601(iso, NULL) : NULL;
	if (parsed == NULL)
		return NULL;

	utc = g_date_time_to_utc(parsed);
	return g_date_time_format(utc, "%Y%m%dT%H%M%SZ");
}

static const gchar *
member_text(
	JsonObject  *object,
	const gchar *key
){
	JsonNode *node = json_object_get_member(object, key);

	if (node == NULL || json_node_get_value_type(node) != G_TYPE_STRING)
		return NULL;

	return json_node_get_string(node);
}

static void
ics_event(
	GString     *out,
	JsonObject  *occurrence,
	const gchar *stamp
){
	g_autofree gchar *start = ics_time(member_text(occurrence, "at"));
	g_autofree gchar *end = ics_time(member_text(occurrence, "latest"));
	g_autofree gchar *uid_raw = NULL;
	g_autofree gchar *uid = NULL;
	g_autofree gchar *summary = NULL;
	g_autofree gchar *description = NULL;
	g_autofree gchar *line = NULL;
	GString *about;
	const gchar *task = member_text(occurrence, "task");
	const gchar *reason = member_text(occurrence, "reason");
	const gchar *detail = member_text(occurrence, "detail");
	gboolean manual = g_strcmp0(member_text(occurrence, "kind"),
	                            "manual") == 0;

	if (start == NULL)
		return;

	/*
	 * The same routine at the same time is the same event, whenever it
	 * is exported, so a calendar that imports twice updates rather than
	 * doubles.  A Run now is its own event at the time it started.
	 */
	uid_raw = g_strdup_printf("%s-%s-%s%s@clawtilla",
	                          member_text(occurrence, "source"),
	                          member_text(occurrence, "routine"), start,
	                          manual ? "-manual" : "");
	uid = ics_escape(uid_raw);
	summary = ics_escape(member_text(occurrence, "title"));

	about = g_string_new(NULL);
	g_string_append_printf(about, "Agent: %s\nStatus: %s",
	                       member_text(occurrence, "agent"),
	                       member_text(occurrence, "status"));
	if (reason != NULL)
		g_string_append_printf(about, " (%s)", reason);
	if (detail != NULL)
		g_string_append_printf(about, "\n%s", detail);
	if (task != NULL)
		g_string_append_printf(about, "\nTask: %s", task);
	g_string_append_printf(about, "\nSource: %s%s",
	                       member_text(occurrence, "source"),
	                       manual ? ", run by hand" : "");
	description = ics_escape(about->str);
	g_string_free(about, TRUE);

	ics_line(out, "BEGIN:VEVENT");
	line = g_strdup_printf("UID:%s", uid);
	ics_line(out, line);
	g_free(line);
	line = g_strdup_printf("DTSTAMP:%s", stamp);
	ics_line(out, line);
	g_free(line);
	line = g_strdup_printf("DTSTART:%s", start);
	ics_line(out, line);
	g_free(line);

	/* A jitter window is the event's span; without one it is a moment. */
	if (end != NULL && g_strcmp0(end, start) != 0) {
		line = g_strdup_printf("DTEND:%s", end);
		ics_line(out, line);
		g_free(line);
	}

	line = g_strdup_printf("SUMMARY:%s", summary);
	ics_line(out, line);
	g_free(line);
	line = g_strdup_printf("DESCRIPTION:%s", description);
	ics_line(out, line);
	g_clear_pointer(&line, g_free);
	ics_line(out, "TRANSP:TRANSPARENT");
	ics_line(out, "END:VEVENT");
}

gchar *
clawt_routine_calendar_to_ics(
	JsonObject *reply,
	GDateTime  *stamp
){
	GString *out = g_string_new(NULL);
	g_autoptr(GDateTime) now = NULL;
	g_autofree gchar *stamp_text = NULL;
	g_autofree gchar *zone_line = NULL;
	JsonArray *list;
	guint i;

	g_return_val_if_fail(reply != NULL, NULL);

	now = (stamp != NULL) ? g_date_time_to_utc(stamp)
	                      : g_date_time_new_now_utc();
	stamp_text = g_date_time_format(now, "%Y%m%dT%H%M%SZ");

	ics_line(out, "BEGIN:VCALENDAR");
	ics_line(out, "VERSION:2.0");
	ics_line(out, "PRODID:-//clawtilla//routine calendar//EN");
	ics_line(out, "CALSCALE:GREGORIAN");
	ics_line(out, "METHOD:PUBLISH");
	ics_line(out, "X-WR-CALNAME:clawtilla routines");

	if (member_text(reply, "time_zone") != NULL) {
		g_autofree gchar *escaped = ics_escape(member_text(reply,
		                                                   "time_zone"));

		zone_line = g_strdup_printf("X-WR-TIMEZONE:%s", escaped);
		ics_line(out, zone_line);
	}

	list = json_object_has_member(reply, "occurrences")
		? json_object_get_array_member(reply, "occurrences") : NULL;

	for (i = 0; list != NULL && i < json_array_get_length(list); i++)
		ics_event(out, json_array_get_object_element(list, i), stamp_text);

	/* Grouped runs are still runs: an importer has no use for a fold. */
	list = json_object_has_member(reply, "groups")
		? json_object_get_array_member(reply, "groups") : NULL;

	for (i = 0; list != NULL && i < json_array_get_length(list); i++) {
		JsonObject *group = json_array_get_object_element(list, i);
		JsonArray *members = json_object_get_array_member(group,
		                                                  "occurrences");
		guint k;

		for (k = 0; members != NULL && k < json_array_get_length(members);
		     k++)
			ics_event(out, json_array_get_object_element(members, k),
			          stamp_text);
	}

	ics_line(out, "END:VCALENDAR");

	return g_string_free(out, FALSE);
}
