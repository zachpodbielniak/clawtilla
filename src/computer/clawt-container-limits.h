/*
 * clawt-container-limits.h - Process and memory limits for a podman machine
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
	"{{.HostConfig.PidsLimit}} {{.HostConfig.Memory}} {{.HostConfig.MemorySwap}}"

#define CLAWT_TYPE_CONTAINER_LIMITS (clawt_container_limits_get_type())

/**
 * ClawtContainerLimits:
 *
 * What computer.container.pids_limit, memory and memory_swap say, held
 * as configured. Opaque; build one with clawt_container_limits_new().
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
 * clawt_container_limits_validate:
 * @self: a #ClawtContainerLimits
 * @error: return location for a #GError
 *
 * Refuses anything podman would refuse when it creates or updates the
 * machine -- a size it cannot read, memory under its 6 MiB floor, a swap
 * total without a memory limit or smaller than it, a process limit under
 * -1 -- naming the key, so the refusal can be given where the value
 * arrives rather than at the next start.
 *
 * Returns: %TRUE if podman will accept these limits
 */
gboolean
clawt_container_limits_validate(
	const ClawtContainerLimits  *self,
	GError                     **error
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
 * ("--pids-limit=2048", "--memory=4g", "--memory-swap=8g"), so each can
 * be handed through distrobox's --additional-flags whole.
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
 * @changed: (out) (optional): whether `podman update` was run
 * @error: return location for a #GError
 *
 * Gives an existing machine these limits in place: asks podman what it
 * has, and runs `podman update` for whatever differs. A running machine
 * takes the change at once; a stopped one has it when it next starts.
 * No rebuild, so nothing the agent installed is lost.
 *
 * Blocking, with each podman call bounded; call it from a worker thread
 * such as the one clawt_computer_lifecycle_async() runs a start on.
 *
 * Returns: %TRUE if the machine now has these limits
 */
gboolean
clawt_container_limits_apply(
	const ClawtContainerLimits  *self,
	const gchar                 *connection,
	const gchar                 *name,
	gboolean                    *changed,
	GError                     **error
);

G_END_DECLS
