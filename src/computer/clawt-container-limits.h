/*
 * clawt-container-limits.h - CPU, process and memory limits for a podman machine
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * One value shared by the container and distrobox backends, because both
 * are a podman container underneath and podman spells the limits once.
 * Everything that decides what reaches podman is a pure function of this
 * value, so the arguments are asserted on as data with no podman at all.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * CLAWT_CONTAINER_MEMORY_FLOOR:
 *
 * The smallest memory limit podman accepts, in bytes (6 MiB). A smaller
 * one is refused when it is set rather than when podman reads it.
 */
#define CLAWT_CONTAINER_MEMORY_FLOOR ((gint64)6 * 1024 * 1024)

/**
 * CLAWT_CONTAINER_LIMITS_INSPECT_FORMAT:
 *
 * The `podman inspect --format` template whose output
 * clawt_container_limits_update_args() compares against: the process
 * limit, the memory limit and the memory-plus-swap limit, space
 * separated, all in podman's own units.
 */
#define CLAWT_CONTAINER_LIMITS_INSPECT_FORMAT \
	"{{.HostConfig.PidsLimit}} {{.HostConfig.Memory}} " \
	"{{.HostConfig.MemorySwap}} {{.HostConfig.NanoCpus}} " \
	"{{.HostConfig.CpuShares}} {{.State.Running}} {{.HostConfig.CpusetCpus}}"

/**
 * CLAWT_CONTAINER_CPU_WEIGHT_MIN:
 *
 * The smallest computer.container.cpu_weight: cgroup v2's cpu.weight floor.
 */
#define CLAWT_CONTAINER_CPU_WEIGHT_MIN (1)

/**
 * CLAWT_CONTAINER_CPU_WEIGHT_MAX:
 *
 * The largest computer.container.cpu_weight: cgroup v2's cpu.weight ceiling.
 */
#define CLAWT_CONTAINER_CPU_WEIGHT_MAX (10000)

/**
 * CLAWT_CONTAINER_LIMIT_KEYS:
 *
 * How many keys a #ClawtContainerLimits holds; see
 * clawt_container_limit_key_nth().
 */
#define CLAWT_CONTAINER_LIMIT_KEYS (6)

/**
 * ClawtContainerLimitsOutcome:
 * @CLAWT_CONTAINER_LIMITS_UNCHANGED: the machine already had these limits;
 *   podman was asked one inspect and nothing else
 * @CLAWT_CONTAINER_LIMITS_APPLIED: `podman update` changed a machine that
 *   was not running, so the limits hold from its next start
 * @CLAWT_CONTAINER_LIMITS_APPLIED_LIVE: `podman update` changed the running
 *   machine in place, with no restart and no recreate
 * @CLAWT_CONTAINER_LIMITS_RESTART_REQUIRED: podman refused to change some
 *   of them on the running machine; they are applied again, to the stopped
 *   machine, when it next starts
 *
 * Which path a change of limits took, so it can be said rather than
 * guessed at.
 */
typedef enum {
	CLAWT_CONTAINER_LIMITS_UNCHANGED = 0,
	CLAWT_CONTAINER_LIMITS_APPLIED,
	CLAWT_CONTAINER_LIMITS_APPLIED_LIVE,
	CLAWT_CONTAINER_LIMITS_RESTART_REQUIRED
} ClawtContainerLimitsOutcome;

/**
 * clawt_container_limits_outcome_nick:
 * @outcome: a #ClawtContainerLimitsOutcome
 *
 * The outcome's spelling on the wire: "unchanged", "applied", "live" or
 * "restart-required".
 *
 * Returns: (transfer none): the nick
 */
const gchar *
clawt_container_limits_outcome_nick(
	ClawtContainerLimitsOutcome outcome
);

/**
 * clawt_container_limit_key_nth:
 * @n: an index below %CLAWT_CONTAINER_LIMIT_KEYS
 *
 * The limit keys in the order every surface lists them, relative to an
 * agent: "computer.container.cpus", "computer.container.cpu_weight",
 * "computer.container.cpuset", "computer.container.memory",
 * "computer.container.memory_swap", "computer.container.pids_limit".
 *
 * Returns: (transfer none) (nullable): the key, or %NULL past the end
 */
const gchar *
clawt_container_limit_key_nth(
	guint n
);

/**
 * clawt_container_limit_key_is:
 * @key: (nullable): an agent-relative key
 *
 * Returns: %TRUE if @key is one of the six container limit keys
 */
gboolean
clawt_container_limit_key_is(
	const gchar *key
);

/**
 * clawt_host_cpu_count:
 *
 * How many CPUs this host has online, from /sys/devices/system/cpu/online
 * (g_get_num_processors() when that cannot be read). The bound
 * computer.container.cpus and cpuset are checked against.
 *
 * Returns: the count, at least 1
 */
guint
clawt_host_cpu_count(
	void
);

/**
 * clawt_host_memory_bytes:
 *
 * The host's memory, from /proc/meminfo, for clients that suggest a
 * memory limit from the size of the machine.
 *
 * Returns: the total in bytes, or 0 when it cannot be read
 */
gint64
clawt_host_memory_bytes(
	void
);

#define CLAWT_TYPE_CONTAINER_LIMITS (clawt_container_limits_get_type())

/**
 * ClawtContainerLimits:
 *
 * What computer.container.cpus, cpu_weight, cpuset, memory, memory_swap
 * and pids_limit say, held as configured. Opaque; build one with
 * clawt_container_limits_new().
 */
typedef struct _ClawtContainerLimits ClawtContainerLimits;

GType clawt_container_limits_get_type(void) G_GNUC_CONST;

/**
 * clawt_container_limits_new:
 *
 * No limits at all: podman's own defaults apply.
 *
 * Returns: (transfer full): a new #ClawtContainerLimits
 */
ClawtContainerLimits *
clawt_container_limits_new(
	void
);

/**
 * clawt_container_limits_copy:
 * @self: a #ClawtContainerLimits
 *
 * Returns: (transfer full): a copy of @self
 */
ClawtContainerLimits *
clawt_container_limits_copy(
	const ClawtContainerLimits *self
);

/**
 * clawt_container_limits_free:
 * @self: (nullable): a #ClawtContainerLimits
 */
void
clawt_container_limits_free(
	ClawtContainerLimits *self
);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtContainerLimits, clawt_container_limits_free)

/**
 * clawt_container_limits_set_pids_limit:
 * @self: a #ClawtContainerLimits
 * @pids_limit: 0 for podman's default, -1 for no limit, else the limit
 */
void
clawt_container_limits_set_pids_limit(
	ClawtContainerLimits *self,
	gint64                pids_limit
);

/**
 * clawt_container_limits_get_pids_limit:
 * @self: a #ClawtContainerLimits
 *
 * Returns: the configured process limit; 0 when unset
 */
gint64
clawt_container_limits_get_pids_limit(
	const ClawtContainerLimits *self
);

/**
 * clawt_container_limits_set_memory:
 * @self: a #ClawtContainerLimits
 * @memory: (nullable): a size such as "512m", or %NULL or "" for none
 */
void
clawt_container_limits_set_memory(
	ClawtContainerLimits *self,
	const gchar          *memory
);

/**
 * clawt_container_limits_get_memory:
 * @self: a #ClawtContainerLimits
 *
 * Returns: (transfer none) (nullable): the memory limit as written
 */
const gchar *
clawt_container_limits_get_memory(
	const ClawtContainerLimits *self
);

/**
 * clawt_container_limits_set_memory_swap:
 * @self: a #ClawtContainerLimits
 * @memory_swap: (nullable): a size, "-1", or %NULL or "" for podman's
 *   default
 */
void
clawt_container_limits_set_memory_swap(
	ClawtContainerLimits *self,
	const gchar          *memory_swap
);

/**
 * clawt_container_limits_get_memory_swap:
 * @self: a #ClawtContainerLimits
 *
 * Returns: (transfer none) (nullable): the memory-plus-swap limit as
 *   written
 */
const gchar *
clawt_container_limits_get_memory_swap(
	const ClawtContainerLimits *self
);

/**
 * clawt_container_limits_set_cpus:
 * @self: a #ClawtContainerLimits
 * @cpus: (nullable): a CPU count such as "4" or "2.5", or %NULL or "" for
 *   none
 */
void
clawt_container_limits_set_cpus(
	ClawtContainerLimits *self,
	const gchar          *cpus
);

/**
 * clawt_container_limits_get_cpus:
 * @self: a #ClawtContainerLimits
 *
 * Returns: (transfer none) (nullable): the CPU limit as written
 */
const gchar *
clawt_container_limits_get_cpus(
	const ClawtContainerLimits *self
);

/**
 * clawt_container_limits_set_cpu_weight:
 * @self: a #ClawtContainerLimits
 * @cpu_weight: 0 for podman's default, else a cgroup v2 cpu.weight from
 *   %CLAWT_CONTAINER_CPU_WEIGHT_MIN to %CLAWT_CONTAINER_CPU_WEIGHT_MAX
 */
void
clawt_container_limits_set_cpu_weight(
	ClawtContainerLimits *self,
	gint64                cpu_weight
);

/**
 * clawt_container_limits_get_cpu_weight:
 * @self: a #ClawtContainerLimits
 *
 * Returns: the configured weight; 0 when unset
 */
gint64
clawt_container_limits_get_cpu_weight(
	const ClawtContainerLimits *self
);

/**
 * clawt_container_limits_set_cpuset:
 * @self: a #ClawtContainerLimits
 * @cpuset: (nullable): a CPU list such as "0-7" or "0,2,4-6", or %NULL or
 *   "" for none
 */
void
clawt_container_limits_set_cpuset(
	ClawtContainerLimits *self,
	const gchar          *cpuset
);

/**
 * clawt_container_limits_get_cpuset:
 * @self: a #ClawtContainerLimits
 *
 * Returns: (transfer none) (nullable): the CPU list as written
 */
const gchar *
clawt_container_limits_get_cpuset(
	const ClawtContainerLimits *self
);

/**
 * clawt_container_limits_set_key:
 * @self: a #ClawtContainerLimits
 * @key: one of the six agent-relative limit keys
 * @value: (nullable): the value as written in clawtilla.yaml, or %NULL for
 *   unset
 * @error: return location for a #GError
 *
 * Sets one limit from its configuration spelling, so the resolver, the
 * daemon's agent.set check and the tests all read a key the same way.
 * Only the whole-number keys can fail here (a pids_limit or cpu_weight
 * that is not a number); everything else is judged by
 * clawt_container_limits_validate().
 *
 * Returns: %TRUE if @key is a limit key and @value was read
 */
gboolean
clawt_container_limits_set_key(
	ClawtContainerLimits  *self,
	const gchar           *key,
	const gchar           *value,
	GError               **error
);

/**
 * clawt_container_limits_get_key:
 * @self: a #ClawtContainerLimits
 * @key: one of the six agent-relative limit keys
 *
 * One limit as text: what clawtilla.yaml would hold for it.
 *
 * Returns: (transfer full) (nullable): the value, or %NULL when unset
 */
gchar *
clawt_container_limits_get_key(
	const ClawtContainerLimits *self,
	const gchar                *key
);

/**
 * clawt_container_limits_is_empty:
 * @self: (nullable): a #ClawtContainerLimits
 *
 * Returns: %TRUE when nothing is set, so podman is never asked anything
 */
gboolean
clawt_container_limits_is_empty(
	const ClawtContainerLimits *self
);

/**
 * clawt_container_size_parse:
 * @text: a size as somebody wrote it: "512m", "4g", "1073741824"
 * @bytes: (out) (optional): the size in bytes
 * @error: return location for a #GError
 *
 * Reads a size the way podman does: a whole number and an optional unit
 * -- b, k, m or g, in either case, optionally followed by b ("4gb") --
 * in powers of 1024.
 *
 * A fraction is refused rather than rounded: "1.5g" would mean whatever
 * the rounding said, and a limit has to have exactly one value. The
 * refusal names the whole-number spelling instead.
 *
 * Returns: %TRUE if @text is a size
 */
gboolean
clawt_container_size_parse(
	const gchar  *text,
	gint64       *bytes,
	GError      **error
);

/**
 * clawt_container_cpus_parse:
 * @text: a CPU count as somebody wrote it: "4", "2.5", "0.25"
 * @nano_cpus: (out) (optional): the count in billionths of a CPU, which is
 *   how podman inspect reports it (NanoCpus)
 * @error: return location for a #GError
 *
 * A positive decimal with at most nine places after the point. No sign,
 * no exponent, no leading point: the spellings podman's --cpus would read
 * differently from a person.
 *
 * Returns: %TRUE if @text is a CPU count above zero
 */
gboolean
clawt_container_cpus_parse(
	const gchar  *text,
	gint64       *nano_cpus,
	GError      **error
);

/**
 * clawt_container_cpuset_normalise:
 * @text: a CPU list: "0-7", "0,2,4-6"
 * @host_cpus: CPUs the host has, numbered from 0; 0 for no bound
 * @error: return location for a #GError
 *
 * Reads a CPU list the way the kernel's cpuset.cpus does and writes it
 * back in one canonical form -- ascending, ranges merged ("0-3,8") -- so
 * two spellings of the same set compare equal.
 *
 * Returns: (transfer full) (nullable): the canonical list, or %NULL with
 *   @error set when @text is not a list or names a CPU past @host_cpus
 */
gchar *
clawt_container_cpuset_normalise(
	const gchar  *text,
	guint         host_cpus,
	GError      **error
);

/**
 * clawt_container_cpu_weight_to_shares:
 * @cpu_weight: a cgroup v2 cpu.weight, 1 to 10000
 *
 * The `podman --cpu-shares` value for a weight. podman has no
 * --cpu-weight; it takes shares (2 to 262144) and the OCI runtime turns
 * them into cpu.weight. This is the inverse of the conversion crun and
 * runc use since 2025 (weight = 10^((l^2 + 125 l)/612 - 7/34), l =
 * log2(shares)), so 1 -> 2, 100 -> 1024 (podman's default) and 10000 ->
 * 262144. An older runtime converts linearly instead; the order of
 * weights, which is all cpu.weight means, is the same either way.
 *
 * Returns: the shares, or 0 for a @cpu_weight of 0
 */
gint64
clawt_container_cpu_weight_to_shares(
	gint64 cpu_weight
);

/**
 * clawt_container_limits_validate:
 * @self: a #ClawtContainerLimits
 * @error: return location for a #GError
 *
 * Refuses anything podman would refuse when it creates or updates the
 * machine -- a size it cannot read, memory under its 6 MiB floor, a swap
 * total without a memory limit or smaller than it, a process limit under
 * -1, a CPU count of zero or more than this host has, a weight outside
 * cgroup v2's 1 to 10000, a CPU list naming a CPU this host lacks --
 * naming the key, so the refusal can be given where the value arrives
 * rather than at the next start.
 *
 * The host is the one clawtilla runs on. A machine on a remote podman
 * (computer.container.connection) is checked against this host too,
 * because its own size cannot be known without asking it.
 *
 * Returns: %TRUE if podman will accept these limits
 */
gboolean
clawt_container_limits_validate(
	const ClawtContainerLimits  *self,
	GError                     **error
);

/**
 * clawt_container_limits_validate_for_host:
 * @self: a #ClawtContainerLimits
 * @host_cpus: CPUs the host has; 0 to skip the checks against the host
 * @error: return location for a #GError
 *
 * As clawt_container_limits_validate(), against a host of @host_cpus
 * CPUs rather than this one, so the bound can be tested.
 *
 * Returns: %TRUE if podman will accept these limits on that host
 */
gboolean
clawt_container_limits_validate_for_host(
	const ClawtContainerLimits  *self,
	guint                        host_cpus,
	GError                     **error
);

/**
 * clawt_container_limits_validate_level:
 * @self: the limits one level sets: the fleet's or a team's
 * @host_cpus: as for clawt_container_limits_validate_for_host()
 * @error: return location for a #GError
 *
 * As clawt_container_limits_validate_for_host(), for one level of the
 * three rather than an agent's effective set: memory_swap without memory
 * is allowed here, because the memory may come from another level -- a
 * team's swap total over each member's own memory. Its spelling is still
 * checked, and the pair is judged against each other when this level sets
 * both, and always where an agent ends up with them.
 *
 * Returns: %TRUE if this level's values are ones podman would take
 */
gboolean
clawt_container_limits_validate_level(
	const ClawtContainerLimits  *self,
	guint                        host_cpus,
	GError                     **error
);

/**
 * clawt_container_limits_drop_invalid:
 * @self: a #ClawtContainerLimits
 * @host_cpus: as for clawt_container_limits_validate_for_host()
 * @refusals: (element-type utf8) (nullable): where to add one sentence
 *   per key that was dropped, naming the key
 *
 * A copy of @self with every limit podman would refuse taken out, so a
 * bad value that reached the daemon without passing `config validate` --
 * a hand edit, a team default an agent's own value now contradicts -- is
 * reported and not applied, and the rest still are. Each key is judged on
 * its own first; then memory_swap, the one key judged against another, is
 * dropped if the pair still fails.
 *
 * Returns: (transfer full): the limits that are valid
 */
ClawtContainerLimits *
clawt_container_limits_drop_invalid(
	const ClawtContainerLimits *self,
	guint                       host_cpus,
	GPtrArray                  *refusals
);

/**
 * clawt_container_limits_inspected_running:
 * @inspected: (nullable): what the inspect command printed
 *
 * Returns: %TRUE if @inspected says the machine is running
 */
gboolean
clawt_container_limits_inspected_running(
	const gchar *inspected
);

/**
 * clawt_container_limits_swap_first:
 * @old_memory: (nullable): computer.container.memory as stored
 * @old_swap: (nullable): computer.container.memory_swap as stored
 * @new_memory: (nullable): the memory about to be saved
 * @new_swap: (nullable): the swap total about to be saved
 *
 * The order a client must send the pair in. `agent.set` writes one key at
 * a time and checks it against the other as stored, so the pair has to
 * pass through a valid state: raising memory past the stored swap total,
 * or clearing memory under a stored swap, is refused until the swap moves
 * first -- while lowering both, or setting both from nothing, needs
 * memory first. Asked of clawt_container_limits_validate(), so a client
 * and the daemon cannot disagree; three clients each had a copy of this.
 *
 * Returns: %TRUE when memory_swap has to be sent before memory
 */
gboolean
clawt_container_limits_swap_first(
	const gchar *old_memory,
	const gchar *old_swap,
	const gchar *new_memory,
	const gchar *new_swap
);

/**
 * clawt_container_limits_create_args:
 * @self: (nullable): a #ClawtContainerLimits
 *
 * The limits as `podman create` options, one complete option per word
 * ("--pids-limit=2048", "--memory=4g", "--memory-swap=8g", "--cpus=4",
 * "--cpu-shares=1024", "--cpuset-cpus=0-7"), so each can be handed
 * through distrobox's --additional-flags whole.
 *
 * Returns: (transfer full) (array zero-terminated=1): the options; empty
 *   when nothing is set
 */
GStrv
clawt_container_limits_create_args(
	const ClawtContainerLimits *self
);

/**
 * clawt_container_limits_podman_prefix:
 * @podman: the podman binary
 * @connection: (nullable): the machine's podman connection: a URI with a
 *   scheme (unix://, ssh://, tcp://), a named connection, or %NULL for
 *   the local podman
 *
 * The start of every podman command line aimed at one machine: the
 * binary and how to reach the podman that owns it.
 *
 * Returns: (transfer full) (array zero-terminated=1): the prefix
 */
GStrv
clawt_container_limits_podman_prefix(
	const gchar *podman,
	const gchar *connection
);

/**
 * clawt_container_limits_find_podman:
 *
 * The podman command line tool, found the way every other binary the
 * daemon runs is found: PATH, then /usr/bin/podman.
 *
 * Returns: (transfer full) (nullable): its path, or %NULL when there is
 *   none
 */
gchar *
clawt_container_limits_find_podman(
	void
);

/**
 * clawt_container_limits_inspect_argv:
 * @podman: the podman binary
 * @connection: (nullable): as for clawt_container_limits_podman_prefix()
 * @name: the container
 *
 * Returns: (transfer full) (array zero-terminated=1): the command that
 *   prints the limits the container has now
 */
GStrv
clawt_container_limits_inspect_argv(
	const gchar *podman,
	const gchar *connection,
	const gchar *name
);

/**
 * clawt_container_limits_update_args:
 * @self: a #ClawtContainerLimits
 * @inspected: (nullable): what the inspect command printed, or %NULL to
 *   apply every limit that is set
 *
 * The `podman update` options for whatever differs between @self and the
 * machine as it is. A limit left unset is never touched: podman cannot
 * tell its own default from a value somebody chose, so clearing a key
 * leaves the last value in place until the machine is rebuilt.
 *
 * Returns: (transfer full) (array zero-terminated=1): the options; empty
 *   when the machine already has these limits
 */
GStrv
clawt_container_limits_update_args(
	const ClawtContainerLimits *self,
	const gchar                *inspected
);

/**
 * clawt_container_limits_update_argv:
 * @podman: the podman binary
 * @connection: (nullable): as for clawt_container_limits_podman_prefix()
 * @name: the container
 * @options: (array zero-terminated=1): from
 *   clawt_container_limits_update_args()
 *
 * Returns: (transfer full) (array zero-terminated=1) (nullable): the whole
 *   `podman update` command, or %NULL when @options is empty
 */
GStrv
clawt_container_limits_update_argv(
	const gchar         *podman,
	const gchar         *connection,
	const gchar         *name,
	const gchar * const *options
);

/**
 * clawt_container_limits_apply:
 * @self: a #ClawtContainerLimits
 * @connection: (nullable): as for clawt_container_limits_podman_prefix()
 * @name: the container, which must already exist
 * @outcome: (out) (optional): which path the change took
 * @note: (out) (optional) (transfer full) (nullable): one sentence saying
 *   what was changed and how, or why podman would not; %NULL when nothing
 *   changed
 * @error: return location for a #GError
 *
 * Gives an existing machine these limits in place: asks podman what it
 * has, and runs `podman update` for whatever differs. A running machine
 * takes the change at once (%CLAWT_CONTAINER_LIMITS_APPLIED_LIVE); a
 * stopped one has it when it next starts (%CLAWT_CONTAINER_LIMITS_APPLIED).
 * No rebuild, so nothing the agent installed is lost.
 *
 * When podman refuses the update on a *running* machine -- a cpuset the
 * cgroup cannot take while tasks run, a rootless cgroup v1 host -- each
 * change is tried on its own (memory and memory_swap together), the ones
 * podman takes are kept, and the rest are reported as
 * %CLAWT_CONTAINER_LIMITS_RESTART_REQUIRED with podman's reason in @note.
 * That is not a failure: the next start applies them to the stopped
 * machine, and fails then, loudly, if podman still says no. A refusal on a
 * machine that is not running *is* a failure, because there is no later
 * to defer it to.
 *
 * Blocking, with each podman call bounded; call it from a worker thread
 * such as the one clawt_computer_lifecycle_async() runs a start on.
 *
 * Returns: %TRUE if the machine has these limits, or will at its next
 *   start
 */
gboolean
clawt_container_limits_apply(
	const ClawtContainerLimits   *self,
	const gchar                  *connection,
	const gchar                  *name,
	ClawtContainerLimitsOutcome  *outcome,
	gchar                       **note,
	GError                      **error
);

G_END_DECLS
