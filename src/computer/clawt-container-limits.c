/*
 * clawt-container-limits.c - Process and memory limits for a podman machine
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "computer/clawt-container-limits.h"
#include "computer/clawt-wake.h"

#include <string.h>

/*
 * How long one podman call may take. inspect and update are local
 * metadata operations; this bounds a podman socket that has gone quiet
 * rather than the work itself.
 */
#define PODMAN_CALL_TIMEOUT_SECONDS (60)

struct _ClawtContainerLimits {
	gint64  pids_limit;
	gchar  *memory;
	gchar  *memory_swap;
};

G_DEFINE_BOXED_TYPE(ClawtContainerLimits, clawt_container_limits,
                    clawt_container_limits_copy, clawt_container_limits_free)

ClawtContainerLimits *
clawt_container_limits_new(
	void
){
	return g_new0(ClawtContainerLimits, 1);
}

ClawtContainerLimits *
clawt_container_limits_copy(
	const ClawtContainerLimits *self
){
	ClawtContainerLimits *copy;

	g_return_val_if_fail(self != NULL, NULL);

	copy = clawt_container_limits_new();
	copy->pids_limit = self->pids_limit;
	copy->memory = g_strdup(self->memory);
	copy->memory_swap = g_strdup(self->memory_swap);

	return copy;
}

void
clawt_container_limits_free(
	ClawtContainerLimits *self
){
	if (self == NULL)
		return;

	g_free(self->memory);
	g_free(self->memory_swap);
	g_free(self);
}

/*
 * Trimmed and lowered, and NULL for blank: "" and an absent key have to
 * mean the same thing, or clearing a field in a client would store a
 * value podman refuses.
 */
static gchar *
normalise_size(
	const gchar *text
){
	g_autofree gchar *copy = NULL;

	if (text == NULL)
		return NULL;

	copy = g_ascii_strdown(text, -1);
	g_strstrip(copy);

	if (*copy == '\0')
		return NULL;

	return g_steal_pointer(&copy);
}

void
clawt_container_limits_set_pids_limit(
	ClawtContainerLimits *self,
	gint64                pids_limit
){
	g_return_if_fail(self != NULL);

	self->pids_limit = pids_limit;
}

gint64
clawt_container_limits_get_pids_limit(
	const ClawtContainerLimits *self
){
	g_return_val_if_fail(self != NULL, 0);

	return self->pids_limit;
}

void
clawt_container_limits_set_memory(
	ClawtContainerLimits *self,
	const gchar          *memory
){
	g_return_if_fail(self != NULL);

	g_free(self->memory);
	self->memory = normalise_size(memory);
}

const gchar *
clawt_container_limits_get_memory(
	const ClawtContainerLimits *self
){
	g_return_val_if_fail(self != NULL, NULL);

	return self->memory;
}

void
clawt_container_limits_set_memory_swap(
	ClawtContainerLimits *self,
	const gchar          *memory_swap
){
	g_return_if_fail(self != NULL);

	g_free(self->memory_swap);
	self->memory_swap = normalise_size(memory_swap);
}

const gchar *
clawt_container_limits_get_memory_swap(
	const ClawtContainerLimits *self
){
	g_return_val_if_fail(self != NULL, NULL);

	return self->memory_swap;
}

gboolean
clawt_container_limits_is_empty(
	const ClawtContainerLimits *self
){
	return self == NULL ||
	       (self->pids_limit == 0 && self->memory == NULL &&
	        self->memory_swap == NULL);
}

gboolean
clawt_container_size_parse(
	const gchar  *text,
	gint64       *bytes,
	GError      **error
){
	const gchar *p;
	guint64 number;
	guint64 multiplier;
	gchar unit;

	if (text == NULL || *text == '\0') {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		                    "no size given");
		return FALSE;
	}

	/*
	 * Digits first, by hand rather than with strtoull(), which would take
	 * a sign, leading space and a hexadecimal prefix -- three spellings
	 * podman reads differently or not at all.
	 */
	number = 0;

	for (p = text; g_ascii_isdigit(*p); p++) {
		if (number > (G_MAXUINT64 - 9) / 10) {
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
			            "'%s' is too large to be a size", text);
			return FALSE;
		}

		number = number * 10 + (guint64)(*p - '0');
	}

	if (p == text) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "'%s' is not a size: write a whole number with an "
		            "optional unit, such as 512m or 4g", text);
		return FALSE;
	}

	/* A fraction is named, because it is the one people actually write. */
	if (*p == '.' || *p == ',') {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "'%s' is not a whole number: a limit needs exactly one "
		            "value, so write it in a smaller unit (1536m rather than "
		            "1.5g)", text);
		return FALSE;
	}

	unit = g_ascii_tolower(*p);
	multiplier = 1;

	switch (unit) {
	case '\0':
	case 'b':
		multiplier = 1;
		break;
	case 'k':
		multiplier = 1024;
		break;
	case 'm':
		multiplier = 1024 * 1024;
		break;
	case 'g':
		multiplier = 1024 * 1024 * 1024;
		break;
	default:
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "'%s' has a unit podman does not read: use b, k, m or g "
		            "(powers of 1024), as in 512m or 4g", text);
		return FALSE;
	}

	if (unit != '\0') {
		p++;

		/* "4gb" is what podman's own help writes, so it is accepted. */
		if (unit != 'b' && g_ascii_tolower(*p) == 'b')
			p++;
	}

	if (*p != '\0') {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "'%s' is not a size: write a whole number with an "
		            "optional unit, such as 512m or 4g", text);
		return FALSE;
	}

	if (number > (guint64)G_MAXINT64 / multiplier) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "'%s' is too large to be a size", text);
		return FALSE;
	}

	if (bytes != NULL)
		*bytes = (gint64)(number * multiplier);

	return TRUE;
}

gboolean
clawt_container_limits_validate(
	const ClawtContainerLimits  *self,
	GError                     **error
){
	g_autoptr(GError) local = NULL;
	gint64 memory;
	gint64 swap;

	g_return_val_if_fail(self != NULL, FALSE);

	if (self->pids_limit < -1) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
		            "computer.container.pids_limit is %" G_GINT64_FORMAT
		            ": it takes 0 for podman's default, -1 for no limit, or "
		            "the most processes the machine may hold",
		            self->pids_limit);
		return FALSE;
	}

	memory = 0;

	if (self->memory != NULL) {
		if (!clawt_container_size_parse(self->memory, &memory, &local)) {
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
			            "computer.container.memory: %s", local->message);
			return FALSE;
		}

		/*
		 * podman's own floor, refused here so the refusal names the key
		 * rather than arriving later as a libpod error from a create.
		 */
		if (memory < CLAWT_CONTAINER_MEMORY_FLOOR) {
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
			            "computer.container.memory is %s, under the 6m "
			            "podman will accept", self->memory);
			return FALSE;
		}
	}

	if (self->memory_swap == NULL)
		return TRUE;

	/*
	 * podman refuses a swap total with no memory limit to total against,
	 * and it is easy to set one without the other from a form.
	 */
	if (self->memory == NULL) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
		                    "computer.container.memory_swap needs "
		                    "computer.container.memory as well: it is memory "
		                    "and swap together, so it means nothing without "
		                    "the memory it adds to");
		return FALSE;
	}

	if (g_strcmp0(self->memory_swap, "-1") == 0)
		return TRUE;

	if (!clawt_container_size_parse(self->memory_swap, &swap, &local)) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
		            "computer.container.memory_swap: %s (or -1 for unlimited "
		            "swap)", local->message);
		return FALSE;
	}

	if (swap < memory) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
		            "computer.container.memory_swap is %s, less than memory "
		            "(%s): it is the total of memory and swap, so it cannot "
		            "be smaller. Set it equal to memory for no swap at all",
		            self->memory_swap, self->memory);
		return FALSE;
	}

	return TRUE;
}

gboolean
clawt_container_limits_swap_first(
	const gchar *old_memory,
	const gchar *old_swap,
	const gchar *new_memory,
	const gchar *new_swap
){
	g_autoptr(ClawtContainerLimits) memory_first = NULL;
	g_autoptr(ClawtContainerLimits) swap_first = NULL;

	/* Memory first is the natural order; keep it whenever it passes. */
	memory_first = clawt_container_limits_new();
	clawt_container_limits_set_memory(memory_first, new_memory);
	clawt_container_limits_set_memory_swap(memory_first, old_swap);

	if (clawt_container_limits_validate(memory_first, NULL))
		return FALSE;

	swap_first = clawt_container_limits_new();
	clawt_container_limits_set_memory(swap_first, old_memory);
	clawt_container_limits_set_memory_swap(swap_first, new_swap);

	return clawt_container_limits_validate(swap_first, NULL);
}

/*
 * One option, spelled as a single word: distrobox splits its flags on
 * whitespace and passes each word with --additional-flags, so
 * "--memory 4g" as two words would become two separate flags, the second
 * of which podman reads as the image.
 */
static void
add_option(
	GPtrArray   *out,
	const gchar *name,
	const gchar *value
){
	g_ptr_array_add(out, g_strdup_printf("--%s=%s", name, value));
}

GStrv
clawt_container_limits_create_args(
	const ClawtContainerLimits *self
){
	GPtrArray *out;

	out = g_ptr_array_new();

	if (self != NULL && self->pids_limit != 0) {
		g_autofree gchar *value =
			g_strdup_printf("%" G_GINT64_FORMAT, self->pids_limit);

		add_option(out, "pids-limit", value);
	}

	if (self != NULL && self->memory != NULL)
		add_option(out, "memory", self->memory);

	if (self != NULL && self->memory_swap != NULL)
		add_option(out, "memory-swap", self->memory_swap);

	g_ptr_array_add(out, NULL);

	return (GStrv)g_ptr_array_free(out, FALSE);
}

GStrv
clawt_container_limits_podman_prefix(
	const gchar *podman,
	const gchar *connection
){
	GPtrArray *out;

	g_return_val_if_fail(podman != NULL, NULL);

	out = g_ptr_array_new();
	g_ptr_array_add(out, g_strdup(podman));

	/*
	 * Whichever podman owns the machine. A URI is passed as --url, so a
	 * remote podman (ssh://, tcp://) is reached the same way podomation
	 * reaches it; a bare word is a named connection from
	 * `podman system connection`.
	 */
	if (connection != NULL && *connection != '\0') {
		if (strstr(connection, "://") != NULL) {
			g_ptr_array_add(out, g_strdup("--url"));
		} else {
			g_ptr_array_add(out, g_strdup("--connection"));
		}

		g_ptr_array_add(out, g_strdup(connection));
	}

	g_ptr_array_add(out, NULL);

	return (GStrv)g_ptr_array_free(out, FALSE);
}

/* @prefix's words, then @rest's, as a new argv. */
static GStrv
join_argv(
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
clawt_container_limits_inspect_argv(
	const gchar *podman,
	const gchar *connection,
	const gchar *name
){
	g_auto(GStrv) prefix = NULL;
	const gchar *rest[6];

	g_return_val_if_fail(podman != NULL, NULL);
	g_return_val_if_fail(name != NULL, NULL);

	prefix = clawt_container_limits_podman_prefix(podman, connection);

	rest[0] = "container";
	rest[1] = "inspect";
	rest[2] = "--format";
	rest[3] = CLAWT_CONTAINER_LIMITS_INSPECT_FORMAT;
	rest[4] = name;
	rest[5] = NULL;

	return join_argv(prefix, rest);
}

/*
 * The three numbers the inspect template prints, or FALSE when the text
 * is not that -- in which case every configured limit is applied rather
 * than guessed at.
 */
static gboolean
parse_inspected(
	gchar       *inspected,
	gint64      *pids,
	gint64      *memory,
	gint64      *swap
){
	g_auto(GStrv) words = NULL;
	gchar *end;
	guint count;
	guint i;
	gint64 values[3];

	if (inspected == NULL)
		return FALSE;

	words = g_strsplit_set(g_strstrip(inspected), " \t\n", -1);
	count = 0;

	for (i = 0; words[i] != NULL && count < 3; i++) {
		if (*words[i] == '\0')
			continue;

		values[count] = g_ascii_strtoll(words[i], &end, 10);

		if (end == words[i] || *end != '\0')
			return FALSE;

		count++;
	}

	if (count != 3)
		return FALSE;

	*pids = values[0];
	*memory = values[1];
	*swap = values[2];

	return TRUE;
}

GStrv
clawt_container_limits_update_args(
	const ClawtContainerLimits *self,
	const gchar                *inspected
){
	GPtrArray *out;
	gint64 have_pids;
	gint64 have_memory;
	gint64 have_swap;
	gboolean known;
	gboolean memory_changed;

	g_return_val_if_fail(self != NULL, NULL);

	have_pids = 0;
	have_memory = 0;
	have_swap = 0;

	{
		/* parse_inspected() strips a copy; the copy is freed here. */
		g_autofree gchar *copy = g_strdup(inspected);

		known = parse_inspected(copy, &have_pids, &have_memory, &have_swap);
	}

	out = g_ptr_array_new();

	if (self->pids_limit != 0) {
		/*
		 * podman reports "no limit" as 0 or -1 depending on version and
		 * cgroup driver; both mean the same as a configured -1.
		 */
		gboolean same = known &&
		                (have_pids == self->pids_limit ||
		                 (self->pids_limit == -1 && have_pids <= 0));

		if (!same) {
			g_autofree gchar *value =
				g_strdup_printf("%" G_GINT64_FORMAT, self->pids_limit);

			add_option(out, "pids-limit", value);
		}
	}

	memory_changed = FALSE;

	if (self->memory != NULL) {
		gint64 want = 0;

		if (!clawt_container_size_parse(self->memory, &want, NULL) ||
		    !known || have_memory != want) {
			add_option(out, "memory", self->memory);
			memory_changed = TRUE;
		}
	}

	if (self->memory_swap != NULL) {
		gint64 want = 0;
		gboolean same;

		/*
		 * Sent again whenever memory is, even if the total matched: podman
		 * checks the pair, and a new memory limit above the old swap total
		 * is refused unless the total moves in the same call.
		 */
		if (g_strcmp0(self->memory_swap, "-1") == 0)
			same = known && have_swap <= 0;
		else
			same = known &&
			       clawt_container_size_parse(self->memory_swap, &want,
			                                  NULL) &&
			       have_swap == want;

		if (!same || memory_changed)
			add_option(out, "memory-swap", self->memory_swap);
	}

	g_ptr_array_add(out, NULL);

	return (GStrv)g_ptr_array_free(out, FALSE);
}

GStrv
clawt_container_limits_update_argv(
	const gchar         *podman,
	const gchar         *connection,
	const gchar         *name,
	const gchar * const *options
){
	g_auto(GStrv) prefix = NULL;
	g_auto(GStrv) with_verb = NULL;
	g_autoptr(GPtrArray) rest = NULL;
	gsize i;

	g_return_val_if_fail(podman != NULL, NULL);
	g_return_val_if_fail(name != NULL, NULL);

	if (options == NULL || options[0] == NULL)
		return NULL;

	prefix = clawt_container_limits_podman_prefix(podman, connection);
	rest = g_ptr_array_new();

	g_ptr_array_add(rest, (gpointer)"update");

	for (i = 0; options[i] != NULL; i++)
		g_ptr_array_add(rest, (gpointer)options[i]);

	g_ptr_array_add(rest, (gpointer)name);
	g_ptr_array_add(rest, NULL);

	with_verb = join_argv(prefix, (const gchar * const *)rest->pdata);

	return g_steal_pointer(&with_verb);
}

/*
 * podman, found the way every other binary the daemon runs is found:
 * PATH, then the install location. NULL when there is none, and the
 * caller says so.
 */
static gchar *
find_podman(
	void
){
	gchar *found;

	found = g_find_program_in_path("podman");

	if (found == NULL && g_file_test("/usr/bin/podman",
	                                 G_FILE_TEST_IS_EXECUTABLE))
		found = g_strdup("/usr/bin/podman");

	return found;
}

/* A podman call that must exit 0, with its own words as the error. */
static gboolean
run_podman(
	GStrv     argv,
	gchar   **stdout_text,
	GError  **error
){
	g_autofree gchar *errors = NULL;
	g_autofree gchar *line = NULL;
	gint exit_status;

	exit_status = -1;

	if (!clawt_run_argv_bounded((const gchar * const *)argv,
	                            PODMAN_CALL_TIMEOUT_SECONDS, &exit_status,
	                            stdout_text, &errors, error))
		return FALSE;

	if (exit_status == 0)
		return TRUE;

	line = clawt_command_line_join((const gchar * const *)argv);

	g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_COMPUTER_PROVISION,
	            "`%s` exited with status %d: %s", line, exit_status,
	            (errors != NULL && *g_strstrip(errors) != '\0')
	            ? errors : "it gave no reason");

	if (stdout_text != NULL)
		g_clear_pointer(stdout_text, g_free);

	return FALSE;
}

gboolean
clawt_container_limits_apply(
	const ClawtContainerLimits  *self,
	const gchar                 *connection,
	const gchar                 *name,
	gboolean                    *changed,
	GError                     **error
){
	g_autofree gchar *podman = NULL;
	g_autofree gchar *inspected = NULL;
	g_auto(GStrv) inspect = NULL;
	g_auto(GStrv) options = NULL;
	g_auto(GStrv) update = NULL;
	g_autoptr(GError) local = NULL;

	g_return_val_if_fail(name != NULL, FALSE);

	if (changed != NULL)
		*changed = FALSE;

	/* Nothing configured is nothing to ask podman about. */
	if (clawt_container_limits_is_empty(self))
		return TRUE;

	if (!clawt_container_limits_validate(self, error))
		return FALSE;

	podman = find_podman();

	if (podman == NULL) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
		                    "computer.container limits are set, and applying "
		                    "them needs the podman command, which is not on "
		                    "PATH or in /usr/bin. Install podman (Fedora: "
		                    "podman), or clear the limits");
		return FALSE;
	}

	/*
	 * What the machine has now. A failed inspect is not fatal: every
	 * configured limit is then applied, which is the same request with
	 * nothing skipped.
	 */
	inspect = clawt_container_limits_inspect_argv(podman, connection, name);

	if (!run_podman(inspect, &inspected, &local)) {
		g_debug("container %s: could not read its limits (%s); applying "
		        "all of them", name, local->message);
		g_clear_error(&local);
	}

	options = clawt_container_limits_update_args(self, inspected);
	update = clawt_container_limits_update_argv(
		podman, connection, name, (const gchar * const *)options);

	if (update == NULL)
		return TRUE;

	if (!run_podman(update, NULL, error)) {
		g_prefix_error(error, "the machine's limits could not be applied: ");
		return FALSE;
	}

	if (changed != NULL)
		*changed = TRUE;

	return TRUE;
}
