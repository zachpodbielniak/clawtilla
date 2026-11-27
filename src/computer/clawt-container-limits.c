/*
 * clawt-container-limits.c - CPU, process and memory limits for a podman machine
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "computer/clawt-container-limits.h"
#include "computer/clawt-wake.h"

#include <math.h>
#include <string.h>

/*
 * How long one podman call may take. inspect and update are local
 * metadata operations; this bounds a podman socket that has gone quiet
 * rather than the work itself.
 */
#define PODMAN_CALL_TIMEOUT_SECONDS (60)

/* The most CPUs a cpuset may name; past any machine podman runs on. */
#define CPUSET_MAX_CPUS (8192)

struct _ClawtContainerLimits {
	gint64  pids_limit;
	gchar  *memory;
	gchar  *memory_swap;
	gchar  *cpus;
	gint64  cpu_weight;
	gchar  *cpuset;
};

/* The keys, in the order every surface lists them. */
static const gchar *const limit_keys[CLAWT_CONTAINER_LIMIT_KEYS] = {
	"computer.container.cpus",
	"computer.container.cpu_weight",
	"computer.container.cpuset",
	"computer.container.memory",
	"computer.container.memory_swap",
	"computer.container.pids_limit"
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
	copy->cpus = g_strdup(self->cpus);
	copy->cpu_weight = self->cpu_weight;
	copy->cpuset = g_strdup(self->cpuset);

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
	g_free(self->cpus);
	g_free(self->cpuset);
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

void
clawt_container_limits_set_cpus(
	ClawtContainerLimits *self,
	const gchar          *cpus
){
	g_return_if_fail(self != NULL);

	g_free(self->cpus);
	self->cpus = normalise_size(cpus);
}

const gchar *
clawt_container_limits_get_cpus(
	const ClawtContainerLimits *self
){
	g_return_val_if_fail(self != NULL, NULL);

	return self->cpus;
}

void
clawt_container_limits_set_cpu_weight(
	ClawtContainerLimits *self,
	gint64                cpu_weight
){
	g_return_if_fail(self != NULL);

	self->cpu_weight = cpu_weight;
}

gint64
clawt_container_limits_get_cpu_weight(
	const ClawtContainerLimits *self
){
	g_return_val_if_fail(self != NULL, 0);

	return self->cpu_weight;
}

void
clawt_container_limits_set_cpuset(
	ClawtContainerLimits *self,
	const gchar          *cpuset
){
	g_return_if_fail(self != NULL);

	g_free(self->cpuset);
	self->cpuset = normalise_size(cpuset);
}

const gchar *
clawt_container_limits_get_cpuset(
	const ClawtContainerLimits *self
){
	g_return_val_if_fail(self != NULL, NULL);

	return self->cpuset;
}

gboolean
clawt_container_limits_is_empty(
	const ClawtContainerLimits *self
){
	return self == NULL ||
	       (self->pids_limit == 0 && self->memory == NULL &&
	        self->memory_swap == NULL && self->cpus == NULL &&
	        self->cpu_weight == 0 && self->cpuset == NULL);
}

const gchar *
clawt_container_limits_outcome_nick(
	ClawtContainerLimitsOutcome outcome
){
	/* Every outcome named, no default: -Wswitch catches the next one. */
	switch (outcome) {
	case CLAWT_CONTAINER_LIMITS_UNCHANGED:
		return "unchanged";
	case CLAWT_CONTAINER_LIMITS_APPLIED:
		return "applied";
	case CLAWT_CONTAINER_LIMITS_APPLIED_LIVE:
		return "live";
	case CLAWT_CONTAINER_LIMITS_RESTART_REQUIRED:
		return "restart-required";
	}

	return "unchanged";
}

const gchar *
clawt_container_limit_key_nth(
	guint n
){
	return (n < G_N_ELEMENTS(limit_keys)) ? limit_keys[n] : NULL;
}

gboolean
clawt_container_limit_key_is(
	const gchar *key
){
	guint i;

	if (key == NULL)
		return FALSE;

	for (i = 0; i < G_N_ELEMENTS(limit_keys); i++)
		if (g_strcmp0(limit_keys[i], key) == 0)
			return TRUE;

	return FALSE;
}

/*
 * A whole number for one of the integer keys, or FALSE with the key
 * named. Blank is 0, which is "unset" for both of them.
 */
static gboolean
read_whole_number(
	const gchar  *key,
	const gchar  *value,
	gint64       *out,
	GError      **error
){
	g_autofree gchar *copy = NULL;

	*out = 0;

	if (value == NULL)
		return TRUE;

	copy = g_strstrip(g_strdup(value));

	if (*copy == '\0')
		return TRUE;

	if (!g_ascii_string_to_signed(copy, 10, G_MININT64, G_MAXINT64, out,
	                              NULL)) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
		            "%s is '%s', which is not a whole number", key, value);
		return FALSE;
	}

	return TRUE;
}

gboolean
clawt_container_limits_set_key(
	ClawtContainerLimits  *self,
	const gchar           *key,
	const gchar           *value,
	GError               **error
){
	gint64 number;

	g_return_val_if_fail(self != NULL, FALSE);
	g_return_val_if_fail(key != NULL, FALSE);

	if (g_strcmp0(key, "computer.container.cpus") == 0) {
		clawt_container_limits_set_cpus(self, value);
	} else if (g_strcmp0(key, "computer.container.cpuset") == 0) {
		clawt_container_limits_set_cpuset(self, value);
	} else if (g_strcmp0(key, "computer.container.memory") == 0) {
		clawt_container_limits_set_memory(self, value);
	} else if (g_strcmp0(key, "computer.container.memory_swap") == 0) {
		clawt_container_limits_set_memory_swap(self, value);
	} else if (g_strcmp0(key, "computer.container.cpu_weight") == 0) {
		if (!read_whole_number(key, value, &number, error))
			return FALSE;

		clawt_container_limits_set_cpu_weight(self, number);
	} else if (g_strcmp0(key, "computer.container.pids_limit") == 0) {
		if (!read_whole_number(key, value, &number, error))
			return FALSE;

		clawt_container_limits_set_pids_limit(self, number);
	} else {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "%s is not a container limit", key);
		return FALSE;
	}

	return TRUE;
}

gchar *
clawt_container_limits_get_key(
	const ClawtContainerLimits *self,
	const gchar                *key
){
	g_return_val_if_fail(self != NULL, NULL);
	g_return_val_if_fail(key != NULL, NULL);

	if (g_strcmp0(key, "computer.container.cpus") == 0)
		return g_strdup(self->cpus);

	if (g_strcmp0(key, "computer.container.cpuset") == 0)
		return g_strdup(self->cpuset);

	if (g_strcmp0(key, "computer.container.memory") == 0)
		return g_strdup(self->memory);

	if (g_strcmp0(key, "computer.container.memory_swap") == 0)
		return g_strdup(self->memory_swap);

	if (g_strcmp0(key, "computer.container.cpu_weight") == 0)
		return (self->cpu_weight != 0)
		       ? g_strdup_printf("%" G_GINT64_FORMAT, self->cpu_weight)
		       : NULL;

	if (g_strcmp0(key, "computer.container.pids_limit") == 0)
		return (self->pids_limit != 0)
		       ? g_strdup_printf("%" G_GINT64_FORMAT, self->pids_limit)
		       : NULL;

	return NULL;
}

/* ── The host ────────────────────────────────────────────────────── */

/*
 * Every CPU in a kernel CPU list ("0-3,8"), marked in @mask, which holds
 * CPUSET_MAX_CPUS entries. FALSE when @text is not such a list.
 */
static gboolean
cpu_list_mark(
	const gchar *text,
	gboolean    *mask,
	guint       *highest,
	GError     **error
){
	g_auto(GStrv) parts = NULL;
	guint i;
	gboolean any;

	any = FALSE;
	*highest = 0;
	parts = g_strsplit(text, ",", -1);

	for (i = 0; parts[i] != NULL; i++) {
		gchar *part = g_strstrip(parts[i]);
		gchar *dash;
		guint64 first;
		guint64 last;
		guint64 cpu;

		dash = strchr(part, '-');

		if (dash != NULL)
			*dash = '\0';

		/*
		 * By hand rather than strtoul(), which takes a sign and leading
		 * space: "-1" must not read as a CPU.
		 */
		if (!g_ascii_string_to_unsigned(part, 10, 0, CPUSET_MAX_CPUS - 1,
		                                &first, NULL) ||
		    (dash != NULL &&
		     !g_ascii_string_to_unsigned(g_strstrip(dash + 1), 10, 0,
		                                 CPUSET_MAX_CPUS - 1, &last,
		                                 NULL))) {
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
			            "'%s' is not a CPU list: write CPU numbers and "
			            "ranges from 0, such as 0-7 or 0,2,4-6", text);
			return FALSE;
		}

		if (dash == NULL)
			last = first;

		if (last < first) {
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
			            "'%s' is not a CPU list: the range %" G_GUINT64_FORMAT
			            "-%" G_GUINT64_FORMAT " runs backwards", text,
			            first, last);
			return FALSE;
		}

		for (cpu = first; cpu <= last; cpu++)
			mask[cpu] = TRUE;

		if ((guint)last > *highest)
			*highest = (guint)last;

		any = TRUE;
	}

	if (!any) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "'%s' names no CPUs", text);
		return FALSE;
	}

	return TRUE;
}

guint
clawt_host_cpu_count(
	void
){
	g_autofree gchar *online = NULL;
	g_autofree gboolean *mask = NULL;
	guint highest;
	guint count;
	guint i;

	/*
	 * The online list rather than g_get_num_processors(), which is this
	 * process's affinity: a daemon started under taskset would otherwise
	 * call a 20-CPU host a 4-CPU one and refuse every limit above that.
	 */
	if (g_file_get_contents("/sys/devices/system/cpu/online", &online, NULL,
	                        NULL)) {
		mask = g_new0(gboolean, CPUSET_MAX_CPUS);

		if (cpu_list_mark(g_strstrip(online), mask, &highest, NULL)) {
			count = 0;

			for (i = 0; i <= highest; i++)
				if (mask[i])
					count++;

			if (count > 0)
				return count;
		}
	}

	return MAX(g_get_num_processors(), 1);
}

gint64
clawt_host_memory_bytes(
	void
){
	g_autofree gchar *meminfo = NULL;
	const gchar *line;
	guint64 kib;

	if (!g_file_get_contents("/proc/meminfo", &meminfo, NULL, NULL))
		return 0;

	line = strstr(meminfo, "MemTotal:");

	if (line == NULL)
		return 0;

	line += strlen("MemTotal:");

	while (*line == ' ' || *line == '\t')
		line++;

	kib = g_ascii_strtoull(line, NULL, 10);

	return (gint64)MIN(kib, (guint64)G_MAXINT64 / 1024) * 1024;
}

/* ── CPU values ──────────────────────────────────────────────────── */

gboolean
clawt_container_cpus_parse(
	const gchar  *text,
	gint64       *nano_cpus,
	GError      **error
){
	const gchar *p;
	gint64 whole;
	gint64 fraction;
	guint places;

	if (text == NULL || *text == '\0') {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		                    "no CPU count given");
		return FALSE;
	}

	/*
	 * Digits by hand, as for a size: no sign, no exponent, no hex, and
	 * no leading point -- ".5" is what podman reads and a person
	 * misreads, so it is written 0.5.
	 */
	whole = 0;

	for (p = text; g_ascii_isdigit(*p); p++) {
		if (whole > 1000000) {
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
			            "'%s' is too many CPUs", text);
			return FALSE;
		}

		whole = whole * 10 + (*p - '0');
	}

	if (p == text) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "'%s' is not a CPU count: write a number such as 4 or "
		            "2.5", text);
		return FALSE;
	}

	fraction = 0;
	places = 0;

	if (*p == '.') {
		for (p++; g_ascii_isdigit(*p); p++) {
			if (places == 9) {
				g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
				            "'%s' has more decimal places than podman "
				            "keeps (nine)", text);
				return FALSE;
			}

			fraction = fraction * 10 + (*p - '0');
			places++;
		}

		if (places == 0) {
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
			            "'%s' is not a CPU count: nothing follows the "
			            "point", text);
			return FALSE;
		}
	}

	if (*p != '\0') {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "'%s' is not a CPU count: write a number such as 4 or "
		            "2.5", text);
		return FALSE;
	}

	for (; places < 9; places++)
		fraction *= 10;

	if (whole == 0 && fraction == 0) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "'%s' CPUs is no CPU at all: leave it unset for no "
		            "limit", text);
		return FALSE;
	}

	if (nano_cpus != NULL)
		*nano_cpus = whole * 1000000000 + fraction;

	return TRUE;
}

gchar *
clawt_container_cpuset_normalise(
	const gchar  *text,
	guint         host_cpus,
	GError      **error
){
	g_autofree gboolean *mask = NULL;
	g_autoptr(GString) out = NULL;
	guint highest;
	guint i;

	if (text == NULL || *text == '\0') {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		                    "no CPU list given");
		return NULL;
	}

	mask = g_new0(gboolean, CPUSET_MAX_CPUS);

	if (!cpu_list_mark(text, mask, &highest, error))
		return NULL;

	if (host_cpus > 0 && highest >= host_cpus) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "'%s' names CPU %u, and this host has %u (0-%u)", text,
		            highest, host_cpus, host_cpus - 1);
		return NULL;
	}

	/* Ascending runs, a run of one written as a single number. */
	out = g_string_new(NULL);

	for (i = 0; i <= highest; i++) {
		guint start;

		if (!mask[i])
			continue;

		start = i;

		while (i + 1 <= highest && mask[i + 1])
			i++;

		if (out->len > 0)
			g_string_append_c(out, ',');

		if (start == i)
			g_string_append_printf(out, "%u", start);
		else
			g_string_append_printf(out, "%u-%u", start, i);
	}

	return g_string_free(g_steal_pointer(&out), FALSE);
}

gint64
clawt_container_cpu_weight_to_shares(
	gint64 cpu_weight
){
	gdouble exponent;
	gdouble l;

	if (cpu_weight <= 0)
		return 0;

	cpu_weight = CLAMP(cpu_weight, CLAWT_CONTAINER_CPU_WEIGHT_MIN,
	                   CLAWT_CONTAINER_CPU_WEIGHT_MAX);

	/*
	 * The runtime computes weight = 10^e with e = (l^2 + 125 l)/612 -
	 * 7/34 and l = log2(shares). Solved for l: l^2 + 125 l - 612 (log10
	 * weight + 7/34) = 0, the positive root. The ends come out exact --
	 * 1 -> l = 1, 100 -> l = 10, 10000 -> l = 18 -- and everything
	 * between is rounded to the nearest whole share.
	 */
	exponent = log10((gdouble)cpu_weight) + 7.0 / 34.0;
	l = (-125.0 + sqrt(125.0 * 125.0 + 4.0 * 612.0 * exponent)) / 2.0;

	return CLAMP((gint64)llround(pow(2.0, l)), 2, 262144);
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

/*
 * The three CPU keys against a host of @host_cpus (0: no bound), each
 * refusal naming its key.
 */
static gboolean
validate_cpu(
	const ClawtContainerLimits  *self,
	guint                        host_cpus,
	GError                     **error
){
	g_autoptr(GError) local = NULL;

	if (self->cpus != NULL) {
		gint64 nano = 0;

		if (!clawt_container_cpus_parse(self->cpus, &nano, &local)) {
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
			            "computer.container.cpus: %s", local->message);
			return FALSE;
		}

		/*
		 * More than the host has is not a limit -- it is a number that
		 * looks like one and caps nothing -- and podman refuses it on
		 * some versions, at the start rather than here.
		 */
		if (host_cpus > 0 && nano > (gint64)host_cpus * 1000000000) {
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
			            "computer.container.cpus is %s, more than the %u "
			            "CPUs this host has", self->cpus, host_cpus);
			return FALSE;
		}
	}

	if (self->cpu_weight != 0 &&
	    (self->cpu_weight < CLAWT_CONTAINER_CPU_WEIGHT_MIN ||
	     self->cpu_weight > CLAWT_CONTAINER_CPU_WEIGHT_MAX)) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
		            "computer.container.cpu_weight is %" G_GINT64_FORMAT
		            ": it takes a cgroup v2 cpu.weight from %d to %d (100 "
		            "is the default share), or 0 for podman's default",
		            self->cpu_weight, CLAWT_CONTAINER_CPU_WEIGHT_MIN,
		            CLAWT_CONTAINER_CPU_WEIGHT_MAX);
		return FALSE;
	}

	if (self->cpuset != NULL) {
		g_autofree gchar *canonical =
			clawt_container_cpuset_normalise(self->cpuset, host_cpus,
			                                 &local);

		if (canonical == NULL) {
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
			            "computer.container.cpuset: %s", local->message);
			return FALSE;
		}
	}

	return TRUE;
}

/* The three memory and process keys, each refusal naming its key. */
static gboolean
validate_memory(
	const ClawtContainerLimits  *self,
	GError                     **error
){
	g_autoptr(GError) local = NULL;
	gint64 memory;
	gint64 swap;

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
clawt_container_limits_validate_for_host(
	const ClawtContainerLimits  *self,
	guint                        host_cpus,
	GError                     **error
){
	g_return_val_if_fail(self != NULL, FALSE);

	return validate_memory(self, error) &&
	       validate_cpu(self, host_cpus, error);
}

gboolean
clawt_container_limits_validate(
	const ClawtContainerLimits  *self,
	GError                     **error
){
	g_return_val_if_fail(self != NULL, FALSE);

	/* Nothing CPU-shaped set: no reason to read /sys at all. */
	if (self->cpus == NULL && self->cpuset == NULL)
		return clawt_container_limits_validate_for_host(self, 0, error);

	return clawt_container_limits_validate_for_host(
		self, clawt_host_cpu_count(), error);
}

gboolean
clawt_container_limits_validate_level(
	const ClawtContainerLimits  *self,
	guint                        host_cpus,
	GError                     **error
){
	g_autoptr(ClawtContainerLimits) without = NULL;
	g_autoptr(GError) local = NULL;

	g_return_val_if_fail(self != NULL, FALSE);

	if (self->memory_swap == NULL || self->memory != NULL)
		return clawt_container_limits_validate_for_host(self, host_cpus,
		                                                error);

	/* A swap total alone: its spelling here, the pair where it lands. */
	if (g_strcmp0(self->memory_swap, "-1") != 0 &&
	    !clawt_container_size_parse(self->memory_swap, NULL, &local)) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
		            "computer.container.memory_swap: %s (or -1 for unlimited "
		            "swap)", local->message);
		return FALSE;
	}

	without = clawt_container_limits_copy(self);
	clawt_container_limits_set_memory_swap(without, NULL);

	return clawt_container_limits_validate_for_host(without, host_cpus,
	                                                error);
}

ClawtContainerLimits *
clawt_container_limits_drop_invalid(
	const ClawtContainerLimits *self,
	guint                       host_cpus,
	GPtrArray                  *refusals
){
	ClawtContainerLimits *kept;
	guint i;

	g_return_val_if_fail(self != NULL, NULL);

	kept = clawt_container_limits_new();

	/*
	 * Each key on its own first, in a set holding nothing else -- except
	 * memory_swap, which only means anything beside memory and so is
	 * judged with it, below.
	 */
	for (i = 0; i < G_N_ELEMENTS(limit_keys); i++) {
		const gchar *key = limit_keys[i];
		g_autofree gchar *value = NULL;
		g_autoptr(ClawtContainerLimits) alone = NULL;
		g_autoptr(GError) why = NULL;

		if (g_strcmp0(key, "computer.container.memory_swap") == 0)
			continue;

		value = clawt_container_limits_get_key(self, key);

		if (value == NULL)
			continue;

		alone = clawt_container_limits_new();
		(void)clawt_container_limits_set_key(alone, key, value, NULL);

		if (!clawt_container_limits_validate_for_host(alone, host_cpus,
		                                              &why)) {
			if (refusals != NULL)
				g_ptr_array_add(refusals, g_strdup(why->message));
			continue;
		}

		(void)clawt_container_limits_set_key(kept, key, value, NULL);
	}

	if (self->memory_swap != NULL) {
		g_autoptr(GError) why = NULL;

		clawt_container_limits_set_memory_swap(kept, self->memory_swap);

		if (!clawt_container_limits_validate_for_host(kept, host_cpus,
		                                              &why)) {
			if (refusals != NULL)
				g_ptr_array_add(refusals, g_strdup(why->message));
			clawt_container_limits_set_memory_swap(kept, NULL);
		}
	}

	return kept;
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

/*
 * The CPU limits as options, in the spelling podman takes: the count as
 * written, the weight as the shares it stands for, and the CPU list in
 * its canonical form, so it compares equal to what inspect reports.
 */
static void
add_cpu_options(
	GPtrArray                  *out,
	const ClawtContainerLimits *self
){
	if (self->cpus != NULL)
		add_option(out, "cpus", self->cpus);

	if (self->cpu_weight != 0) {
		g_autofree gchar *shares = g_strdup_printf(
			"%" G_GINT64_FORMAT,
			clawt_container_cpu_weight_to_shares(self->cpu_weight));

		add_option(out, "cpu-shares", shares);
	}

	if (self->cpuset != NULL) {
		g_autofree gchar *canonical =
			clawt_container_cpuset_normalise(self->cpuset, 0, NULL);

		add_option(out, "cpuset-cpus",
		           canonical != NULL ? canonical : self->cpuset);
	}
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

	if (self != NULL)
		add_cpu_options(out, self);

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
 * What the inspect template printed, read back.
 *
 * Older output -- the three memory and process numbers alone, which is
 * what this printed before the CPU keys -- still reads: the CPU half is
 * then unknown, and every CPU limit set is applied rather than guessed
 * at. Anything that does not start with three numbers is unknown
 * altogether, and every configured limit is applied.
 */
typedef struct {
	gboolean  known;
	gint64    pids;
	gint64    memory;
	gint64    swap;
	gboolean  cpu_known;
	gint64    nano_cpus;
	gint64    shares;
	gboolean  running;
	gchar    *cpuset;
} Inspected;

static void
inspected_clear(
	Inspected *inspected
){
	g_clear_pointer(&inspected->cpuset, g_free);
}

static void
parse_inspected(
	const gchar *text,
	Inspected   *out
){
	g_autofree gchar *copy = NULL;
	g_auto(GStrv) words = NULL;
	g_autoptr(GPtrArray) tokens = NULL;
	gint64 values[5] = { 0, 0, 0, 0, 0 };
	gchar *end;
	guint i;

	memset(out, 0, sizeof(*out));

	if (text == NULL)
		return;

	copy = g_strdup(text);
	words = g_strsplit_set(g_strstrip(copy), " \t\n", -1);
	tokens = g_ptr_array_new();

	for (i = 0; words[i] != NULL; i++)
		if (*words[i] != '\0')
			g_ptr_array_add(tokens, words[i]);

	if (tokens->len < 3)
		return;

	for (i = 0; i < MIN(tokens->len, 5u); i++) {
		const gchar *word = g_ptr_array_index(tokens, i);

		values[i] = g_ascii_strtoll(word, &end, 10);

		if (end == word || *end != '\0') {
			/* The first three must be numbers; the CPU two may not be. */
			if (i < 3)
				return;

			break;
		}
	}

	out->known = TRUE;
	out->pids = values[0];
	out->memory = values[1];
	out->swap = values[2];

	if (i < 5 || tokens->len < 6)
		return;

	out->cpu_known = TRUE;
	out->nano_cpus = values[3];
	out->shares = values[4];
	out->running = g_strcmp0(g_ptr_array_index(tokens, 5), "true") == 0;

	/* An unset cpuset prints nothing, so its absence is "none". */
	if (tokens->len > 6)
		out->cpuset = g_strdup(g_ptr_array_index(tokens, 6));
}

gboolean
clawt_container_limits_inspected_running(
	const gchar *inspected
){
	Inspected parsed;
	gboolean running;

	parse_inspected(inspected, &parsed);
	running = parsed.running;
	inspected_clear(&parsed);

	return running;
}

GStrv
clawt_container_limits_update_args(
	const ClawtContainerLimits *self,
	const gchar                *inspected
){
	GPtrArray *out;
	Inspected have;
	gboolean memory_changed;

	g_return_val_if_fail(self != NULL, NULL);

	parse_inspected(inspected, &have);
	out = g_ptr_array_new();

	if (self->pids_limit != 0) {
		/*
		 * podman reports "no limit" as 0 or -1 depending on version and
		 * cgroup driver; both mean the same as a configured -1.
		 */
		gboolean same = have.known &&
		                (have.pids == self->pids_limit ||
		                 (self->pids_limit == -1 && have.pids <= 0));

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
		    !have.known || have.memory != want) {
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
			same = have.known && have.swap <= 0;
		else
			same = have.known &&
			       clawt_container_size_parse(self->memory_swap, &want,
			                                  NULL) &&
			       have.swap == want;

		if (!same || memory_changed)
			add_option(out, "memory-swap", self->memory_swap);
	}

	/*
	 * The CPU keys, each compared in podman's own unit: billionths of a
	 * CPU, shares, and the CPU list in canonical form -- "0-3" and
	 * "0,1,2,3" are the same set and must not cost an update.
	 */
	if (self->cpus != NULL) {
		gint64 want = 0;

		if (!clawt_container_cpus_parse(self->cpus, &want, NULL) ||
		    !have.cpu_known || have.nano_cpus != want)
			add_option(out, "cpus", self->cpus);
	}

	if (self->cpu_weight != 0) {
		gint64 shares = clawt_container_cpu_weight_to_shares(self->cpu_weight);

		if (!have.cpu_known || have.shares != shares) {
			g_autofree gchar *value =
				g_strdup_printf("%" G_GINT64_FORMAT, shares);

			add_option(out, "cpu-shares", value);
		}
	}

	if (self->cpuset != NULL) {
		g_autofree gchar *want =
			clawt_container_cpuset_normalise(self->cpuset, 0, NULL);
		g_autofree gchar *now = (have.cpuset != NULL)
			? clawt_container_cpuset_normalise(have.cpuset, 0, NULL)
			: NULL;

		if (want == NULL || !have.cpu_known || g_strcmp0(want, now) != 0)
			add_option(out, "cpuset-cpus",
			           want != NULL ? want : self->cpuset);
	}

	inspected_clear(&have);
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
 * caller says so. Public so the container computer's adoption check
 * finds the same binary the limits do; see the header.
 */
gchar *
clawt_container_limits_find_podman(
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

/*
 * The update options split into what podman can be asked for alone:
 * each option by itself, except memory and memory-swap, which podman
 * checks as a pair and so must travel together.
 */
static GPtrArray *
option_groups(
	const gchar * const *options
){
	GPtrArray *groups;
	GPtrArray *memory;
	gsize i;

	groups = g_ptr_array_new_with_free_func((GDestroyNotify)g_strfreev);
	memory = g_ptr_array_new_with_free_func(g_free);

	for (i = 0; options != NULL && options[i] != NULL; i++) {
		if (g_str_has_prefix(options[i], "--memory=") ||
		    g_str_has_prefix(options[i], "--memory-swap=")) {
			g_ptr_array_add(memory, g_strdup(options[i]));
		} else {
			GStrv one = g_new0(gchar *, 2);

			one[0] = g_strdup(options[i]);
			g_ptr_array_add(groups, one);
		}
	}

	if (memory->len > 0) {
		g_ptr_array_add(memory, NULL);
		g_ptr_array_add(groups, g_ptr_array_free(memory, FALSE));
	} else {
		g_ptr_array_unref(memory);
	}

	return groups;
}

/* podman's complaint, its first line, for a sentence about it. */
static gchar *
first_line(
	const gchar *text
){
	const gchar *newline;

	if (text == NULL)
		return g_strdup("it gave no reason");

	newline = strchr(text, '\n');

	return (newline != NULL) ? g_strndup(text, newline - text)
	                         : g_strdup(text);
}

gboolean
clawt_container_limits_apply(
	const ClawtContainerLimits   *self,
	const gchar                  *connection,
	const gchar                  *name,
	ClawtContainerLimitsOutcome  *outcome,
	gchar                       **note,
	GError                      **error
){
	g_autofree gchar *podman = NULL;
	g_autofree gchar *inspected = NULL;
	g_autofree gchar *wanted = NULL;
	g_auto(GStrv) inspect = NULL;
	g_auto(GStrv) options = NULL;
	g_auto(GStrv) update = NULL;
	g_autoptr(GError) local = NULL;
	g_autoptr(GPtrArray) groups = NULL;
	g_autoptr(GPtrArray) taken = NULL;
	g_autoptr(GPtrArray) refused = NULL;
	gboolean running;
	guint i;

	g_return_val_if_fail(name != NULL, FALSE);

	if (outcome != NULL)
		*outcome = CLAWT_CONTAINER_LIMITS_UNCHANGED;

	if (note != NULL)
		*note = NULL;

	/* Nothing configured is nothing to ask podman about. */
	if (clawt_container_limits_is_empty(self))
		return TRUE;

	if (!clawt_container_limits_validate(self, error))
		return FALSE;

	podman = clawt_container_limits_find_podman();

	if (podman == NULL) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
		                    "computer.container limits are set, and applying "
		                    "them needs the podman command, which is not on "
		                    "PATH or in /usr/bin. Install podman (Fedora: "
		                    "podman), or clear the limits");
		return FALSE;
	}

	/*
	 * What the machine has now, and whether it is running. A failed
	 * inspect is not fatal: every configured limit is then applied, which
	 * is the same request with nothing skipped -- and the machine is
	 * taken as stopped, so a refusal is reported rather than deferred.
	 */
	inspect = clawt_container_limits_inspect_argv(podman, connection, name);

	if (!run_podman(inspect, &inspected, &local)) {
		g_debug("container %s: could not read its limits (%s); applying "
		        "all of them", name, local->message);
		g_clear_error(&local);
	}

	running = clawt_container_limits_inspected_running(inspected);
	options = clawt_container_limits_update_args(self, inspected);
	update = clawt_container_limits_update_argv(
		podman, connection, name, (const gchar * const *)options);

	if (update == NULL)
		return TRUE;

	wanted = g_strjoinv(" ", options);

	if (run_podman(update, NULL, &local)) {
		if (outcome != NULL)
			*outcome = running ? CLAWT_CONTAINER_LIMITS_APPLIED_LIVE
			                   : CLAWT_CONTAINER_LIMITS_APPLIED;

		if (note != NULL)
			*note = g_strdup_printf(
				running ? "%s changed in place on the running machine, "
				          "with no restart"
				        : "%s set on the stopped machine, before its start",
				wanted);

		return TRUE;
	}

	/*
	 * A machine that is not running has no later to defer a refusal to:
	 * the start is what is waiting on these limits, and starting without
	 * them would be the quiet version of not having set them.
	 */
	if (!running) {
		g_propagate_prefixed_error(error, g_steal_pointer(&local),
		                           "the machine's limits could not be "
		                           "applied: ");
		return FALSE;
	}

	/*
	 * Running, and podman said no to the lot. Each change on its own, so
	 * the one podman cannot make live -- a cpuset, say -- does not hold
	 * back the memory limit somebody set because memory is short now.
	 */
	g_debug("container %s: `podman update %s` refused on the running "
	        "machine (%s); trying each change alone", name, wanted,
	        local->message);
	g_clear_error(&local);

	groups = option_groups((const gchar * const *)options);
	taken = g_ptr_array_new_with_free_func(g_free);
	refused = g_ptr_array_new_with_free_func(g_free);

	for (i = 0; i < groups->len; i++) {
		GStrv group = g_ptr_array_index(groups, i);
		g_auto(GStrv) one = clawt_container_limits_update_argv(
			podman, connection, name, (const gchar * const *)group);
		g_autofree gchar *words = g_strjoinv(" ", group);
		g_autoptr(GError) why = NULL;

		if (run_podman(one, NULL, &why)) {
			g_ptr_array_add(taken, g_steal_pointer(&words));
		} else {
			g_autofree gchar *reason = first_line(why->message);

			g_ptr_array_add(refused, g_strdup_printf("%s (%s)", words,
			                                         reason));
		}
	}

	if (refused->len == 0) {
		g_ptr_array_add(taken, NULL);

		if (outcome != NULL)
			*outcome = CLAWT_CONTAINER_LIMITS_APPLIED_LIVE;

		if (note != NULL) {
			g_autofree gchar *list = g_strjoinv(" ", (gchar **)taken->pdata);

			*note = g_strdup_printf("%s changed in place on the running "
			                        "machine, with no restart", list);
		}

		return TRUE;
	}

	g_ptr_array_add(refused, NULL);
	g_ptr_array_add(taken, NULL);

	if (outcome != NULL)
		*outcome = CLAWT_CONTAINER_LIMITS_RESTART_REQUIRED;

	if (note != NULL) {
		g_autofree gchar *refusals = g_strjoinv("; ", (gchar **)refused->pdata);
		g_autofree gchar *list = g_strjoinv(" ", (gchar **)taken->pdata);

		*note = (taken->len > 1)
			? g_strdup_printf("restart required: podman would not change "
			                  "%s on the running machine; they are applied "
			                  "when it next starts. %s changed in place",
			                  refusals, list)
			: g_strdup_printf("restart required: podman would not change "
			                  "%s on the running machine; they are applied "
			                  "when it next starts", refusals);
	}

	return TRUE;
}
