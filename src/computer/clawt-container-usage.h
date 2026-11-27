/*
 * clawt-container-usage.h - What a podman machine is using, from podman stats
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The daemon samples every running container computer on an interval --
 * one `podman stats --no-stream` per podman connection, never one per
 * request -- and keeps the last sample on the computer, where agent.list
 * and the Computer tab read it. Everything here is the pure half: the
 * command lines, reading what podman printed, the CPU rate between two
 * samples, the OOM counter in the container's cgroup, and the sentences
 * said about it. The scheduling is the daemon's (daemon-usage.c).
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * CLAWT_CONTAINER_USAGE_STATS_FORMAT:
 *
 * The `podman stats --format` template the sampler asks for: one line
 * per running container, fields separated by '|' -- the full id, the
 * name, podman's own CPU percentage, the cumulative CPU time and the
 * sample's clock (both in nanoseconds, for the rate between samples),
 * memory used, the memory limit (the host's memory when there is none),
 * and the process count. ContainerStats.MemUsage is spelled in full
 * because podman's template wrapper has a MemUsage() method of its own
 * that prints a human string.
 */
#define CLAWT_CONTAINER_USAGE_STATS_FORMAT \
	"{{.ContainerID}}|{{.Name}}|{{.CPU}}|{{.CPUNano}}|{{.SystemNano}}|" \
	"{{.ContainerStats.MemUsage}}|{{.MemLimit}}|{{.PIDs}}"

/**
 * CLAWT_CONTAINER_USAGE_CGROUP_FORMAT:
 *
 * The `podman inspect --format` template that says where each container's
 * cgroup is, for the OOM counter: "name|/user.slice/.../libpod-<id>.scope".
 */
#define CLAWT_CONTAINER_USAGE_CGROUP_FORMAT "{{.Name}}|{{.State.CgroupPath}}"

/**
 * CLAWT_CONTAINER_USAGE_PRESSURE_PERCENT:
 *
 * Memory use, as a percentage of the configured limit, at which a machine
 * counts as sitting at its limit.
 */
#define CLAWT_CONTAINER_USAGE_PRESSURE_PERCENT (95)

#define CLAWT_TYPE_CONTAINER_USAGE (clawt_container_usage_get_type())

/**
 * ClawtContainerUsage:
 * @container_id: podman's full id
 * @name: the container's name
 * @cpu_percent: CPU use as a percentage of one CPU -- 400 is four CPUs
 *   flat out -- over the time since the previous sample, or podman's own
 *   figure for the first
 * @cpu_nano: CPU time used since the container started, in nanoseconds
 * @system_nano: when podman took the sample, in nanoseconds
 * @memory_used: memory in use, in bytes
 * @memory_limit: the memory limit podman reports, in bytes -- the host's
 *   memory when the machine has none
 * @pids: processes and threads in the machine
 * @sampled_at: when clawtilla took the sample, g_get_real_time()
 * @oom_kills: the cgroup's oom_kill count, or -1 when it cannot be read
 *   (a remote podman, or cgroup v1)
 *
 * One sample of one machine.
 */
typedef struct {
	gchar   *container_id;
	gchar   *name;
	gdouble  cpu_percent;
	gint64   cpu_nano;
	gint64   system_nano;
	gint64   memory_used;
	gint64   memory_limit;
	gint64   pids;
	gint64   sampled_at;
	gint64   oom_kills;
} ClawtContainerUsage;

GType clawt_container_usage_get_type(void) G_GNUC_CONST;

/**
 * clawt_container_usage_copy:
 * @self: a #ClawtContainerUsage
 *
 * Returns: (transfer full): a copy of @self
 */
ClawtContainerUsage *
clawt_container_usage_copy(
	const ClawtContainerUsage *self
);

/**
 * clawt_container_usage_free:
 * @self: (nullable): a #ClawtContainerUsage
 */
void
clawt_container_usage_free(
	ClawtContainerUsage *self
);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtContainerUsage, clawt_container_usage_free)

/**
 * clawt_container_usage_stats_argv:
 * @podman: the podman binary
 * @connection: (nullable): as for clawt_container_limits_podman_prefix()
 *
 * The one command that samples every running container on @connection.
 *
 * Returns: (transfer full) (array zero-terminated=1): the argv
 */
GStrv
clawt_container_usage_stats_argv(
	const gchar *podman,
	const gchar *connection
);

/**
 * clawt_container_usage_parse:
 * @text: (nullable): what the stats command printed
 * @previous: (nullable) (element-type utf8 ClawtContainerUsage): the last
 *   samples, by container name, for the CPU rate
 * @now: the time to stamp each sample with, g_get_real_time()
 *
 * Reads the lines %CLAWT_CONTAINER_USAGE_STATS_FORMAT prints. A line that
 * is not that shape is skipped rather than failing the rest.
 *
 * The CPU percentage is worked out from the CPU time and the clock
 * between this sample and @previous's, when the container is the same
 * one (same id) and the clock moved forward. podman's own figure for a
 * single --no-stream sample is the average since the container started,
 * which for a machine up a week says nothing about the build running now.
 *
 * Returns: (transfer full) (element-type ClawtContainerUsage): the samples
 */
GPtrArray *
clawt_container_usage_parse(
	const gchar *text,
	GHashTable  *previous,
	gint64       now
);

/**
 * clawt_container_usage_cgroup_argv:
 * @podman: the podman binary
 * @connection: (nullable): as for clawt_container_limits_podman_prefix()
 * @names: (array zero-terminated=1): the containers
 *
 * Returns: (transfer full) (array zero-terminated=1): the inspect that
 *   prints where each of @names keeps its cgroup
 */
GStrv
clawt_container_usage_cgroup_argv(
	const gchar         *podman,
	const gchar         *connection,
	const gchar * const *names
);

/**
 * clawt_container_usage_parse_cgroups:
 * @text: (nullable): what the cgroup inspect printed
 *
 * Returns: (transfer full) (element-type utf8 utf8): container name to
 *   cgroup path, relative to the cgroup v2 mount
 */
GHashTable *
clawt_container_usage_parse_cgroups(
	const gchar *text
);

/**
 * clawt_container_usage_read_oom_kills:
 * @cgroup_root: (nullable): where cgroup v2 is mounted, or %NULL for
 *   /sys/fs/cgroup
 * @cgroup_path: (nullable): the container's cgroup under it
 *
 * The oom_kill count in the cgroup's memory.events: how many processes
 * the kernel has killed in it for want of memory. Counted for the whole
 * subtree, so a build's compiler killed in a child cgroup counts too.
 *
 * Returns: the count, or -1 when it cannot be read
 */
gint64
clawt_container_usage_read_oom_kills(
	const gchar *cgroup_root,
	const gchar *cgroup_path
);

/**
 * clawt_container_usage_at_memory_limit:
 * @self: a #ClawtContainerUsage
 * @memory_limit: (nullable): the computer.container.memory the machine
 *   has, as written
 *
 * Whether the machine is using at least
 * %CLAWT_CONTAINER_USAGE_PRESSURE_PERCENT of the memory limit it was
 * given. Judged against the configured limit rather than podman's
 * figure, which is the host's memory when there is none.
 *
 * Returns: %TRUE when the machine is at its limit
 */
gboolean
clawt_container_usage_at_memory_limit(
	const ClawtContainerUsage *self,
	const gchar               *memory_limit
);

/**
 * clawt_container_usage_oom_reason:
 * @memory_limit: (nullable): computer.container.memory, as written
 * @container: the container's name
 * @killed: how many processes were killed since the last look; 0 when
 *   not known
 *
 * The incident's reason for an OOM kill, naming the limit:
 * "killed: out of memory at 8g (container clawt-forge, 1 process)".
 *
 * Returns: (transfer full): the sentence
 */
gchar *
clawt_container_usage_oom_reason(
	const gchar *memory_limit,
	const gchar *container,
	gint64       killed
);

/**
 * clawt_container_usage_format_bytes:
 * @bytes: a size
 *
 * A size the way podman's units write it, in powers of 1024 with one
 * decimal: "7.8g", "512.0m", "900b".
 *
 * Returns: (transfer full): the text
 */
gchar *
clawt_container_usage_format_bytes(
	gint64 bytes
);

/**
 * clawt_container_limits_suggest_cpus:
 * @host_cpus: how many CPUs the host has
 *
 * A quarter of the host, as text a cpus field accepts. A whole number
 * when the quarter is integral ("5" from 20), otherwise the shortest
 * decimal ("0.5" from 2).
 *
 * Returns: (transfer full) (nullable): the suggestion, or %NULL when
 *   @host_cpus is 0
 */
gchar *
clawt_container_limits_suggest_cpus(
	guint host_cpus
);

/**
 * clawt_container_limits_suggest_memory:
 * @host_memory: the host's memory, in bytes
 *
 * A quarter of the host, in the same units as
 * clawt_container_usage_format_bytes().
 *
 * Returns: (transfer full) (nullable): the suggestion, or %NULL when
 *   @host_memory is not positive
 */
gchar *
clawt_container_limits_suggest_memory(
	gint64 host_memory
);

/**
 * clawt_container_limits_inherit_text:
 * @source: (nullable): "agent", "team", "defaults" or "empty"
 * @team: (nullable): the team name when @source is "team"
 * @written: (nullable): the value as written at that level
 *
 * The sentence under a field the agent leaves empty: where the value
 * comes from, and that leaving the field empty keeps it. Nothing when
 * the agent sets the value itself, or when nothing is written.
 *
 * Returns: (transfer full) (nullable): the sentence
 */
gchar *
clawt_container_limits_inherit_text(
	const gchar *source,
	const gchar *team,
	const gchar *written
);

/**
 * clawt_container_limits_inherit_for:
 * @limits: (nullable): the container_limits object
 * @key: one of cpus, cpu_weight, cpuset, memory, memory_swap, pids_limit
 *
 * clawt_container_limits_inherit_text() for one key of @limits. An
 * agent that sets its own value gets nothing, so the field stays empty
 * rather than showing a value a save would pin.
 *
 * Returns: (transfer full) (nullable): the sentence
 */
gchar *
clawt_container_limits_inherit_for(
	JsonObject  *limits,
	const gchar *key
);

/**
 * clawt_container_limits_field_placeholder:
 * @key: one of the six limit keys
 * @limits: (nullable): the container_limits object, for host_cpus and
 *   host_memory
 *
 * The suggestion a field shows and does not submit. cpus and memory
 * are about a quarter of the host when that size is known; the others
 * are the examples the three clients already used.
 *
 * Returns: (transfer full): the placeholder
 */
gchar *
clawt_container_limits_field_placeholder(
	const gchar *key,
	JsonObject  *limits
);

/**
 * clawt_container_limits_summary:
 * @limits: (nullable): the container_limits object
 *
 * The effective limits in the CLI's words: "cpus 4 (team forge),
 * memory 8g (agent)". Empty when nothing is set.
 *
 * Returns: (transfer full): the line, possibly empty
 */
gchar *
clawt_container_limits_summary(
	JsonObject *limits
);

/**
 * clawt_container_usage_summary:
 * @usage: (nullable): the container_usage object
 *
 * "CPU 12.5%, memory 3.1g of 8.0g, 143 pids", with " -- at its memory
 * limit" when at_memory_limit is set. Empty when @usage is %NULL.
 *
 * Returns: (transfer full): the line, possibly empty
 */
gchar *
clawt_container_usage_summary(
	JsonObject *usage
);

/**
 * clawt_container_agent_detail:
 * @agent: (nullable): an agent.list or agent.show object
 *
 * The limits line and the usage line, separated by a newline. %NULL
 * when the agent has neither, so a state line without a sample stays
 * the sentence it was.
 *
 * Returns: (transfer full) (nullable): the text
 */
gchar *
clawt_container_agent_detail(
	JsonObject *agent
);

/**
 * clawt_container_memory_pressure_text:
 * @agent_id: (nullable): the agent
 * @agent: (nullable): an agent.list or agent.show object
 *
 * "agent forge is at its memory limit: 7.8g of 8g used" when
 * container_usage.at_memory_limit is set. The daemon says the same
 * sentence on team.list; this is the one a row can show from the
 * agent object alone.
 *
 * Returns: (transfer full) (nullable): the sentence
 */
gchar *
clawt_container_memory_pressure_text(
	const gchar *agent_id,
	JsonObject  *agent
);

/**
 * clawt_container_oom_notice:
 * @incident: (nullable): one incident.list object
 *
 * "agent forge: killed: out of memory ..." for an unresolved incident
 * of kind out-of-memory. Anything else, including a resolved one, is
 * %NULL.
 *
 * Returns: (transfer full) (nullable): the sentence
 */
gchar *
clawt_container_oom_notice(
	JsonObject *incident
);

G_END_DECLS
