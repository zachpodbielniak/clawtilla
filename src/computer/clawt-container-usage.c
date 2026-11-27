/*
 * clawt-container-usage.c - What a podman machine is using, from podman stats
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "computer/clawt-container-usage.h"

#include <string.h>

G_DEFINE_BOXED_TYPE(ClawtContainerUsage, clawt_container_usage,
                    clawt_container_usage_copy, clawt_container_usage_free)

ClawtContainerUsage *
clawt_container_usage_copy(
	const ClawtContainerUsage *self
){
	ClawtContainerUsage *copy;

	g_return_val_if_fail(self != NULL, NULL);

	copy = g_new0(ClawtContainerUsage, 1);
	*copy = *self;
	copy->container_id = g_strdup(self->container_id);
	copy->name = g_strdup(self->name);

	return copy;
}

void
clawt_container_usage_free(
	ClawtContainerUsage *self
){
	if (self == NULL)
		return;

	g_free(self->container_id);
	g_free(self->name);
	g_free(self);
}

/* @prefix's words, then @rest's, as a new argv. */
static GStrv
argv_join(
	GStrv                prefix,
	const gchar * const *rest
){
	GPtrArray *out;
	gsize i;

	out = g_ptr_array_new();

	for (i = 0; prefix != NULL && prefix[i] != NULL; i++)
		g_ptr_array_add(out, g_strdup(prefix[i]));

	for (i = 0; rest != NULL && rest[i] != NULL; i++)
		g_ptr_array_add(out, g_strdup(rest[i]));

	g_ptr_array_add(out, NULL);

	return (GStrv)g_ptr_array_free(out, FALSE);
}

GStrv
clawt_container_usage_stats_argv(
	const gchar *podman,
	const gchar *connection
){
	g_auto(GStrv) prefix = NULL;
	static const gchar *const rest[] = {
		"stats", "--no-stream", "--no-trunc", "--format",
		CLAWT_CONTAINER_USAGE_STATS_FORMAT, NULL
	};

	g_return_val_if_fail(podman != NULL, NULL);

	prefix = clawt_container_limits_podman_prefix(podman, connection);

	return argv_join(prefix, rest);
}

GStrv
clawt_container_usage_cgroup_argv(
	const gchar         *podman,
	const gchar         *connection,
	const gchar * const *names
){
	g_auto(GStrv) prefix = NULL;
	g_autoptr(GPtrArray) rest = NULL;
	gsize i;

	g_return_val_if_fail(podman != NULL, NULL);

	prefix = clawt_container_limits_podman_prefix(podman, connection);
	rest = g_ptr_array_new();

	g_ptr_array_add(rest, (gpointer)"container");
	g_ptr_array_add(rest, (gpointer)"inspect");
	g_ptr_array_add(rest, (gpointer)"--format");
	g_ptr_array_add(rest, (gpointer)CLAWT_CONTAINER_USAGE_CGROUP_FORMAT);

	for (i = 0; names != NULL && names[i] != NULL; i++)
		g_ptr_array_add(rest, (gpointer)names[i]);

	g_ptr_array_add(rest, NULL);

	return argv_join(prefix, (const gchar * const *)rest->pdata);
}

/* A whole number, or FALSE: podman prints these as plain integers. */
static gboolean
read_int(
	const gchar *text,
	gint64      *out
){
	gchar *end;

	if (text == NULL || *text == '\0')
		return FALSE;

	*out = g_ascii_strtoll(text, &end, 10);

	return end != text && *end == '\0';
}

GPtrArray *
clawt_container_usage_parse(
	const gchar *text,
	GHashTable  *previous,
	gint64       now
){
	GPtrArray *out;
	g_auto(GStrv) lines = NULL;
	gsize i;

	out = g_ptr_array_new_with_free_func(
		(GDestroyNotify)clawt_container_usage_free);

	if (text == NULL)
		return out;

	lines = g_strsplit(text, "\n", -1);

	for (i = 0; lines[i] != NULL; i++) {
		g_auto(GStrv) fields = NULL;
		g_autoptr(ClawtContainerUsage) usage = NULL;
		const ClawtContainerUsage *before;
		gchar *end;

		g_strstrip(lines[i]);

		if (*lines[i] == '\0')
			continue;

		fields = g_strsplit(lines[i], "|", -1);

		if (g_strv_length(fields) != 8)
			continue;

		usage = g_new0(ClawtContainerUsage, 1);
		usage->container_id = g_strdup(g_strstrip(fields[0]));
		usage->name = g_strdup(g_strstrip(fields[1]));
		usage->sampled_at = now;
		usage->oom_kills = -1;

		/* podman's percentage, kept only as the first sample's answer. */
		usage->cpu_percent = g_ascii_strtod(g_strstrip(fields[2]), &end);

		if (end == fields[2] || *end != '\0')
			usage->cpu_percent = 0.0;

		if (*usage->name == '\0' ||
		    !read_int(g_strstrip(fields[3]), &usage->cpu_nano) ||
		    !read_int(g_strstrip(fields[4]), &usage->system_nano) ||
		    !read_int(g_strstrip(fields[5]), &usage->memory_used) ||
		    !read_int(g_strstrip(fields[6]), &usage->memory_limit) ||
		    !read_int(g_strstrip(fields[7]), &usage->pids))
			continue;

		/*
		 * The rate since the last look: CPU time used over wall time
		 * passed, both podman's own clocks, so the poll's own timing
		 * does not enter into it. A different id is a recreated
		 * container whose counter started again from nothing.
		 */
		before = (previous != NULL)
			? g_hash_table_lookup(previous, usage->name) : NULL;

		if (before != NULL &&
		    g_strcmp0(before->container_id, usage->container_id) == 0 &&
		    usage->system_nano > before->system_nano &&
		    usage->cpu_nano >= before->cpu_nano)
			usage->cpu_percent =
				100.0 * (gdouble)(usage->cpu_nano - before->cpu_nano) /
				(gdouble)(usage->system_nano - before->system_nano);

		g_ptr_array_add(out, g_steal_pointer(&usage));
	}

	return out;
}

GHashTable *
clawt_container_usage_parse_cgroups(
	const gchar *text
){
	GHashTable *out;
	g_auto(GStrv) lines = NULL;
	gsize i;

	out = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

	if (text == NULL)
		return out;

	lines = g_strsplit(text, "\n", -1);

	for (i = 0; lines[i] != NULL; i++) {
		gchar *bar = strchr(lines[i], '|');
		gchar *name;
		gchar *path;

		if (bar == NULL)
			continue;

		*bar = '\0';
		name = g_strstrip(lines[i]);
		path = g_strstrip(bar + 1);

		/* inspect prints a leading slash on the name on some versions. */
		if (*name == '/')
			name++;

		if (*name == '\0' || *path != '/')
			continue;

		g_hash_table_replace(out, g_strdup(name), g_strdup(path));
	}

	return out;
}

gint64
clawt_container_usage_read_oom_kills(
	const gchar *cgroup_root,
	const gchar *cgroup_path
){
	g_autofree gchar *file = NULL;
	g_autofree gchar *contents = NULL;
	g_auto(GStrv) lines = NULL;
	gsize i;

	if (cgroup_path == NULL || *cgroup_path != '/' ||
	    strstr(cgroup_path, "..") != NULL)
		return -1;

	file = g_build_filename(cgroup_root != NULL ? cgroup_root
	                                            : "/sys/fs/cgroup",
	                        cgroup_path, "memory.events", NULL);

	if (!g_file_get_contents(file, &contents, NULL, NULL))
		return -1;

	lines = g_strsplit(contents, "\n", -1);

	for (i = 0; lines[i] != NULL; i++) {
		gint64 count;

		if (!g_str_has_prefix(lines[i], "oom_kill "))
			continue;

		if (read_int(g_strstrip(lines[i] + strlen("oom_kill ")), &count))
			return count;
	}

	return -1;
}

gboolean
clawt_container_usage_at_memory_limit(
	const ClawtContainerUsage *self,
	const gchar               *memory_limit
){
	gint64 limit = 0;

	g_return_val_if_fail(self != NULL, FALSE);

	if (memory_limit == NULL ||
	    !clawt_container_size_parse(memory_limit, &limit, NULL) ||
	    limit <= 0)
		return FALSE;

	return self->memory_used >=
	       limit / 100 * CLAWT_CONTAINER_USAGE_PRESSURE_PERCENT;
}

gchar *
clawt_container_usage_format_bytes(
	gint64 bytes
){
	static const gchar units[] = { 'k', 'm', 'g', 't' };
	gdouble value;
	guint unit;

	if (bytes < 1024)
		return g_strdup_printf("%" G_GINT64_FORMAT "b", MAX(bytes, 0));

	value = (gdouble)bytes / 1024.0;
	unit = 0;

	while (value >= 1024.0 && unit + 1 < G_N_ELEMENTS(units)) {
		value /= 1024.0;
		unit++;
	}

	{
		gchar number[G_ASCII_DTOSTR_BUF_SIZE];

		/* g_ascii_formatd: a point whatever the locale says. */
		g_ascii_formatd(number, sizeof(number), "%.1f", value);

		return g_strdup_printf("%s%c", number, units[unit]);
	}
}

gchar *
clawt_container_usage_oom_reason(
	const gchar *memory_limit,
	const gchar *container,
	gint64       killed
){
	g_autofree gchar *count = NULL;

	count = (killed > 0)
		? g_strdup_printf(", %" G_GINT64_FORMAT " process%s killed", killed,
		                  killed == 1 ? "" : "es")
		: g_strdup("");

	/*
	 * The limit by name, as it was written: "out of memory at 8g" is the
	 * sentence that says which setting to look at, where "Killed" alone
	 * reads as a crash.
	 */
	if (memory_limit != NULL && *memory_limit != '\0')
		return g_strdup_printf("killed: out of memory at %s (container "
		                       "%s%s). The limit is "
		                       "computer.container.memory",
		                       memory_limit, container, count);

	return g_strdup_printf("killed: out of memory (container %s%s), with no "
	                       "computer.container.memory set -- the host itself "
	                       "ran short", container, count);
}

/* ── What the clients say ─────────────────────────────────────────── */

static const gchar *
usage_json_string(
	JsonObject  *object,
	const gchar *key
){
	JsonNode *node;
	const gchar *text;

	if (object == NULL || key == NULL ||
	    !json_object_has_member(object, key))
		return "";

	node = json_object_get_member(object, key);

	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) ||
	    json_node_get_value_type(node) != G_TYPE_STRING)
		return "";

	text = json_node_get_string(node);

	return text != NULL ? text : "";
}

static gboolean
usage_json_bool(
	JsonObject  *object,
	const gchar *key
){
	JsonNode *node;

	if (object == NULL || key == NULL ||
	    !json_object_has_member(object, key))
		return FALSE;

	node = json_object_get_member(object, key);

	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) ||
	    json_node_get_value_type(node) != G_TYPE_BOOLEAN)
		return FALSE;

	return json_node_get_boolean(node);
}

static gint64
usage_json_int(
	JsonObject  *object,
	const gchar *key
){
	JsonNode *node;

	if (object == NULL || key == NULL ||
	    !json_object_has_member(object, key))
		return 0;

	node = json_object_get_member(object, key);

	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node))
		return 0;

	if (json_node_get_value_type(node) == G_TYPE_INT64)
		return json_node_get_int(node);

	if (json_node_get_value_type(node) == G_TYPE_DOUBLE)
		return (gint64)json_node_get_double(node);

	return 0;
}

static gdouble
usage_json_double(
	JsonObject  *object,
	const gchar *key
){
	JsonNode *node;

	if (object == NULL || key == NULL ||
	    !json_object_has_member(object, key))
		return 0.0;

	node = json_object_get_member(object, key);

	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node))
		return 0.0;

	if (json_node_get_value_type(node) == G_TYPE_DOUBLE)
		return json_node_get_double(node);

	if (json_node_get_value_type(node) == G_TYPE_INT64)
		return (gdouble)json_node_get_int(node);

	return 0.0;
}

static JsonObject *
usage_json_object(
	JsonObject  *object,
	const gchar *key
){
	JsonNode *node;

	if (object == NULL || key == NULL ||
	    !json_object_has_member(object, key))
		return NULL;

	node = json_object_get_member(object, key);

	if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node))
		return NULL;

	return json_node_get_object(node);
}

gchar *
clawt_container_limits_suggest_cpus(
	guint host_cpus
){
	gchar number[G_ASCII_DTOSTR_BUF_SIZE];
	gchar *end;

	if (host_cpus == 0)
		return NULL;

	/*
	 * A whole quarter stays a whole number: "5", not "5.0". Anything
	 * else keeps the shortest decimal g_ascii_formatd will give, so a
	 * two-CPU host suggests "0.5".
	 */
	g_ascii_formatd(number, sizeof(number), "%.4f",
	                (gdouble)host_cpus / 4.0);
	end = number + strlen(number);

	while (end > number && end[-1] == '0')
		*--end = '\0';

	if (end > number && end[-1] == '.')
		end[-1] = '\0';

	return g_strdup(number);
}

gchar *
clawt_container_limits_suggest_memory(
	gint64 host_memory
){
	if (host_memory <= 0)
		return NULL;

	return clawt_container_usage_format_bytes(host_memory / 4);
}

gchar *
clawt_container_limits_inherit_text(
	const gchar *source,
	const gchar *team,
	const gchar *written
){
	if (written == NULL || *written == '\0')
		return NULL;

	if (g_strcmp0(source, "team") == 0)
		return g_strdup_printf(
			"Inherited from team %s: %s. Leave empty to keep it.",
			(team != NULL && *team != '\0') ? team : "?", written);

	if (g_strcmp0(source, "defaults") == 0)
		return g_strdup_printf(
			"Inherited from the fleet: %s. Leave empty to keep it.",
			written);

	return NULL;
}

gchar *
clawt_container_limits_inherit_for(
	JsonObject  *limits,
	const gchar *key
){
	JsonObject *entry = usage_json_object(limits, key);
	const gchar *own;

	if (entry == NULL)
		return NULL;

	own = usage_json_string(entry, "own");

	if (*own != '\0')
		return NULL;

	return clawt_container_limits_inherit_text(
		usage_json_string(entry, "source"),
		usage_json_string(entry, "team"),
		usage_json_string(entry, "written"));
}

gchar *
clawt_container_limits_field_placeholder(
	const gchar *key,
	JsonObject  *limits
){
	g_autofree gchar *suggest = NULL;

	if (g_strcmp0(key, "cpus") == 0) {
		suggest = clawt_container_limits_suggest_cpus(
			(guint)usage_json_int(limits, "host_cpus"));

		if (suggest != NULL)
			return g_strdup_printf(
				"e.g. %s -- a quarter of this host", suggest);

		return g_strdup("e.g. 2 -- empty for no cap");
	}

	if (g_strcmp0(key, "memory") == 0) {
		suggest = clawt_container_limits_suggest_memory(
			usage_json_int(limits, "host_memory"));

		if (suggest != NULL)
			return g_strdup_printf(
				"e.g. %s -- a quarter of this host", suggest);

		return g_strdup("e.g. 512m or 4g -- empty for no limit");
	}

	if (g_strcmp0(key, "cpu_weight") == 0)
		return g_strdup("1 to 10000 -- empty for the default");

	if (g_strcmp0(key, "cpuset") == 0)
		return g_strdup("e.g. 0-3 -- empty for every CPU");

	if (g_strcmp0(key, "memory_swap") == 0)
		return g_strdup(
			"memory and swap together, e.g. 8g; equal to Memory for no "
			"swap, -1 for unlimited swap");

	if (g_strcmp0(key, "pids_limit") == 0)
		return g_strdup(
			"e.g. 2048 -- empty for podman's default, -1 for no limit");

	return g_strdup("");
}

gchar *
clawt_container_limits_summary(
	JsonObject *limits
){
	static const gchar *const keys[] = {
		"cpus", "cpu_weight", "cpuset", "memory", "memory_swap",
		"pids_limit", NULL
	};
	GString *line = g_string_new(NULL);
	guint i;

	for (i = 0; keys[i] != NULL; i++) {
		JsonObject *entry = usage_json_object(limits, keys[i]);
		const gchar *written;
		const gchar *source;
		const gchar *team;
		g_autofree gchar *from = NULL;

		if (entry == NULL)
			continue;

		written = usage_json_string(entry, "written");

		if (*written == '\0')
			continue;

		source = usage_json_string(entry, "source");
		team = usage_json_string(entry, "team");
		from = (g_strcmp0(source, "team") == 0 && *team != '\0')
			? g_strdup_printf("team %s", team) : g_strdup(source);

		g_string_append_printf(
			line, "%s%s %s (%s%s)",
			line->len > 0 ? ", " : "", keys[i], written, from,
			*usage_json_string(entry, "value") == '\0' ? ", refused" : "");
	}

	return g_string_free(line, FALSE);
}

gchar *
clawt_container_usage_summary(
	JsonObject *usage
){
	gint64 used;
	gint64 limit;
	g_autofree gchar *used_text = NULL;
	g_autofree gchar *limit_text = NULL;

	if (usage == NULL)
		return g_strdup("");

	used = usage_json_int(usage, "memory_used");
	limit = usage_json_int(usage, "memory_limit");
	used_text = clawt_container_usage_format_bytes(used);
	limit_text = clawt_container_usage_format_bytes(limit);

	return g_strdup_printf(
		"CPU %.1f%%, memory %s of %s, %" G_GINT64_FORMAT " pids%s",
		usage_json_double(usage, "cpu_percent"), used_text, limit_text,
		usage_json_int(usage, "pids"),
		usage_json_bool(usage, "at_memory_limit")
			? " -- at its memory limit" : "");
}

gchar *
clawt_container_agent_detail(
	JsonObject *agent
){
	g_autofree gchar *limits = NULL;
	g_autofree gchar *usage = NULL;

	if (agent == NULL)
		return NULL;

	limits = clawt_container_limits_summary(
		usage_json_object(agent, "container_limits"));
	usage = clawt_container_usage_summary(
		usage_json_object(agent, "container_usage"));

	if (*limits == '\0' && *usage == '\0')
		return NULL;

	if (*usage == '\0')
		return g_steal_pointer(&limits);

	if (*limits == '\0')
		return g_steal_pointer(&usage);

	return g_strdup_printf("%s\n%s", limits, usage);
}

gchar *
clawt_container_memory_pressure_text(
	const gchar *agent_id,
	JsonObject  *agent
){
	JsonObject *usage = usage_json_object(agent, "container_usage");
	JsonObject *memory;
	const gchar *written;
	g_autofree gchar *used = NULL;

	if (usage == NULL || !usage_json_bool(usage, "at_memory_limit"))
		return NULL;

	memory = usage_json_object(
		usage_json_object(agent, "container_limits"), "memory");
	written = usage_json_string(memory, "written");

	if (*written == '\0')
		written = "?";

	used = clawt_container_usage_format_bytes(
		usage_json_int(usage, "memory_used"));

	return g_strdup_printf(
		"agent %s is at its memory limit: %s of %s used",
		(agent_id != NULL && *agent_id != '\0') ? agent_id : "?",
		used, written);
}

gchar *
clawt_container_oom_notice(
	JsonObject *incident
){
	const gchar *reason;
	const gchar *agent;

	if (incident == NULL || usage_json_bool(incident, "resolved"))
		return NULL;

	if (g_strcmp0(usage_json_string(incident, "kind"),
	              "out-of-memory") != 0)
		return NULL;

	reason = usage_json_string(incident, "reason");

	if (*reason == '\0')
		return NULL;

	agent = usage_json_string(incident, "agent");

	return g_strdup_printf("agent %s: %s",
	                       *agent != '\0' ? agent : "?", reason);
}
