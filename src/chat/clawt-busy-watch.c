/*
 * clawt-busy-watch.c - A turn whose processes are using the CPU is working
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "chat/clawt-busy-watch.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* One process, as the snapshot holds it. */
typedef struct {
	gint   pid;
	gint   ppid;
	gint64 own;
	gint64 children;
} ProcEntry;

/* One key's last reading. */
typedef struct {
	gint64 ticks;
	gint64 at_usec;
	gint   pid;
} Baseline;

struct _ClawtBusyWatch {
	GObject parent_instance;

	ClawtProcListFunc list;
	ClawtProcReadFunc read;
	gpointer          reader_data;
	GDestroyNotify    reader_notify;

	guint ticks_per_second;

	GArray     *snapshot;   /* ProcEntry */
	GHashTable *baselines;  /* key -> Baseline* */
};

G_DEFINE_FINAL_TYPE(ClawtBusyWatch, clawt_busy_watch, G_TYPE_OBJECT)

/* ── The real /proc ──────────────────────────────────────────────── */

static gchar **
proc_list_real(
	gpointer user_data
){
	g_autoptr(GDir) dir = NULL;
	g_autoptr(GPtrArray) out = NULL;
	const gchar *name;

	(void)user_data;

	dir = g_dir_open("/proc", 0, NULL);

	if (dir == NULL)
		return NULL;

	out = g_ptr_array_new();

	while ((name = g_dir_read_name(dir)) != NULL) {
		if (name[0] < '0' || name[0] > '9')
			continue;

		g_ptr_array_add(out, g_strdup(name));
	}

	g_ptr_array_add(out, NULL);

	return (gchar **)g_ptr_array_free(g_steal_pointer(&out), FALSE);
}

static gchar *
proc_read_real(
	const gchar *pid,
	gpointer     user_data
){
	g_autofree gchar *path = NULL;
	gchar *contents = NULL;

	(void)user_data;

	path = g_build_filename("/proc", pid, "stat", NULL);

	/* A process that exited between the listing and here is ordinary. */
	if (!g_file_get_contents(path, &contents, NULL, NULL))
		return NULL;

	return contents;
}

/* ── Parsing ─────────────────────────────────────────────────────── */

gboolean
clawt_busy_parse_stat(
	const gchar *line,
	gint        *out_ppid,
	gint64      *out_own,
	gint64      *out_children
){
	const gchar *close;
	g_auto(GStrv) fields = NULL;
	guint n;

	g_return_val_if_fail(out_ppid != NULL, FALSE);
	g_return_val_if_fail(out_own != NULL, FALSE);
	g_return_val_if_fail(out_children != NULL, FALSE);

	if (line == NULL)
		return FALSE;

	/*
	 * From the last parenthesis, not the first: a process may call
	 * itself "a) b", and counting fields from the first ')' would read
	 * its state as its parent.
	 */
	close = strrchr(line, ')');

	if (close == NULL)
		return FALSE;

	fields = g_strsplit_set(close + 1, " \n", -1);

	{
		/* Squeeze out the empty strings a run of separators leaves. */
		g_autoptr(GPtrArray) kept = g_ptr_array_new();
		guint i;

		for (i = 0; fields[i] != NULL; i++)
			if (fields[i][0] != '\0')
				g_ptr_array_add(kept, fields[i]);

		n = kept->len;

		/*
		 * After the name: state, ppid, pgrp, session, tty_nr, tpgid,
		 * flags, minflt, cminflt, majflt, cmajflt, utime, stime, cutime,
		 * cstime -- the last four are what this is for.
		 */
		if (n < 15)
			return FALSE;

		*out_ppid = (gint)g_ascii_strtoll(g_ptr_array_index(kept, 1),
			NULL, 10);
		*out_own = g_ascii_strtoll(g_ptr_array_index(kept, 11), NULL, 10) +
			g_ascii_strtoll(g_ptr_array_index(kept, 12), NULL, 10);
		*out_children =
			g_ascii_strtoll(g_ptr_array_index(kept, 13), NULL, 10) +
			g_ascii_strtoll(g_ptr_array_index(kept, 14), NULL, 10);
	}

	return TRUE;
}

/* ── The watch ───────────────────────────────────────────────────── */

ClawtBusyWatch *
clawt_busy_watch_new(
	void
){
	return g_object_new(CLAWT_TYPE_BUSY_WATCH, NULL);
}

void
clawt_busy_watch_set_reader(
	ClawtBusyWatch    *self,
	ClawtProcListFunc  list,
	ClawtProcReadFunc  read,
	gpointer           user_data,
	GDestroyNotify     notify
){
	g_return_if_fail(CLAWT_IS_BUSY_WATCH(self));
	g_return_if_fail((list == NULL) == (read == NULL));

	if (self->reader_notify != NULL)
		self->reader_notify(self->reader_data);

	self->list = list;
	self->read = read;
	self->reader_data = user_data;
	self->reader_notify = notify;
}

void
clawt_busy_watch_set_ticks_per_second(
	ClawtBusyWatch *self,
	guint           ticks
){
	g_return_if_fail(CLAWT_IS_BUSY_WATCH(self));

	self->ticks_per_second = ticks;
}

static guint
ticks_per_second(
	ClawtBusyWatch *self
){
	glong kernel;

	if (self->ticks_per_second > 0)
		return self->ticks_per_second;

	kernel = sysconf(_SC_CLK_TCK);

	return (kernel > 0) ? (guint)kernel : 100;
}

guint
clawt_busy_watch_refresh(
	ClawtBusyWatch *self
){
	g_auto(GStrv) pids = NULL;
	ClawtProcListFunc list;
	ClawtProcReadFunc read;
	guint i;

	g_return_val_if_fail(CLAWT_IS_BUSY_WATCH(self), 0);

	list = (self->list != NULL) ? self->list : proc_list_real;
	read = (self->read != NULL) ? self->read : proc_read_real;

	g_array_set_size(self->snapshot, 0);

	pids = list(self->reader_data);

	for (i = 0; pids != NULL && pids[i] != NULL; i++) {
		g_autofree gchar *line = read(pids[i], self->reader_data);
		ProcEntry entry = { 0 };

		if (!clawt_busy_parse_stat(line, &entry.ppid, &entry.own,
				&entry.children))
			continue;

		entry.pid = (gint)g_ascii_strtoll(pids[i], NULL, 10);
		g_array_append_val(self->snapshot, entry);
	}

	return self->snapshot->len;
}

gint64
clawt_busy_watch_tree_ticks(
	ClawtBusyWatch *self,
	gint            pid,
	guint          *out_processes
){
	g_autoptr(GHashTable) inside = NULL;
	gboolean found = FALSE;
	gboolean grew;
	gint64 total = 0;
	guint count = 0;
	guint i;

	g_return_val_if_fail(CLAWT_IS_BUSY_WATCH(self), -1);

	if (out_processes != NULL)
		*out_processes = 0;

	if (pid <= 0)
		return -1;

	/*
	 * The set of pids in the tree, grown to a fixed point.  The table
	 * is not ordered parent-first -- a reused low pid can be the child
	 * of a high one -- so one pass is not enough.  Bounded by the
	 * snapshot's size, since each pass that grows adds at least one.
	 */
	inside = g_hash_table_new(g_direct_hash, g_direct_equal);
	g_hash_table_add(inside, GINT_TO_POINTER(pid));

	do {
		grew = FALSE;

		for (i = 0; i < self->snapshot->len; i++) {
			ProcEntry *entry = &g_array_index(self->snapshot, ProcEntry, i);

			if (entry->pid == pid || entry->pid <= 0)
				continue;

			if (g_hash_table_contains(inside, GINT_TO_POINTER(entry->pid)))
				continue;

			if (g_hash_table_contains(inside, GINT_TO_POINTER(entry->ppid))) {
				g_hash_table_add(inside, GINT_TO_POINTER(entry->pid));
				grew = TRUE;
			}
		}
	} while (grew);

	for (i = 0; i < self->snapshot->len; i++) {
		ProcEntry *entry = &g_array_index(self->snapshot, ProcEntry, i);

		if (entry->pid == pid) {
			/* The root's reaped children; never its own bookkeeping. */
			total += entry->children;
			found = TRUE;
			continue;
		}

		if (!g_hash_table_contains(inside, GINT_TO_POINTER(entry->pid)))
			continue;

		total += entry->own + entry->children;
		count++;
	}

	if (out_processes != NULL)
		*out_processes = count;

	return found ? total : -1;
}

gboolean
clawt_busy_watch_sample(
	ClawtBusyWatch *self,
	const gchar    *key,
	gint            pid,
	gint64          now_usec,
	guint           threshold_percent,
	gint           *out_percent
){
	Baseline *last;
	gint64 ticks;
	gint64 gained;
	gint64 elapsed;
	gint percent;

	g_return_val_if_fail(CLAWT_IS_BUSY_WATCH(self), FALSE);
	g_return_val_if_fail(key != NULL, FALSE);

	if (out_percent != NULL)
		*out_percent = -1;

	ticks = clawt_busy_watch_tree_ticks(self, pid, NULL);

	if (ticks < 0) {
		/* The process is gone: nothing to compare with next time either. */
		g_hash_table_remove(self->baselines, key);
		return FALSE;
	}

	last = g_hash_table_lookup(self->baselines, key);

	/*
	 * A first reading, or a reading of a different process -- the agent
	 * restarted between two sweeps -- has nothing to compare with.
	 * Treating the whole total as "gained" would call a restart work.
	 */
	if (last == NULL || last->pid != pid || now_usec <= last->at_usec) {
		if (last == NULL) {
			last = g_new0(Baseline, 1);
			g_hash_table_insert(self->baselines, g_strdup(key), last);
		}

		last->ticks = ticks;
		last->at_usec = now_usec;
		last->pid = pid;
		return FALSE;
	}

	gained = ticks - last->ticks;
	elapsed = now_usec - last->at_usec;

	last->ticks = ticks;
	last->at_usec = now_usec;

	/*
	 * Down is not activity.  An exited process's time reappears in its
	 * parent only once the parent waits, so for one sweep the total can
	 * drop; that becomes the baseline and the next sweep compares fairly.
	 */
	if (gained <= 0) {
		if (out_percent != NULL)
			*out_percent = 0;
		return FALSE;
	}

	/*
	 * Percent of one core: ticks over the tick rate is CPU seconds,
	 * over the elapsed wall seconds is a fraction.  Integer arithmetic
	 * in that order, so a threshold has exactly one value.
	 */
	percent = (gint)MIN((gained * 100 * G_USEC_PER_SEC) /
		((gint64)ticks_per_second(self) * elapsed), (gint64)G_MAXINT);

	if (out_percent != NULL)
		*out_percent = percent;

	if (threshold_percent == 0)
		return FALSE;

	return percent >= (gint)threshold_percent;
}

void
clawt_busy_watch_forget(
	ClawtBusyWatch *self,
	const gchar    *key
){
	g_return_if_fail(CLAWT_IS_BUSY_WATCH(self));

	if (key != NULL)
		g_hash_table_remove(self->baselines, key);
}

static void
clawt_busy_watch_finalize(
	GObject *object
){
	ClawtBusyWatch *self = CLAWT_BUSY_WATCH(object);

	if (self->reader_notify != NULL)
		self->reader_notify(self->reader_data);

	g_clear_pointer(&self->snapshot, g_array_unref);
	g_clear_pointer(&self->baselines, g_hash_table_unref);

	G_OBJECT_CLASS(clawt_busy_watch_parent_class)->finalize(object);
}

static void
clawt_busy_watch_class_init(
	ClawtBusyWatchClass *klass
){
	G_OBJECT_CLASS(klass)->finalize = clawt_busy_watch_finalize;
}

static void
clawt_busy_watch_init(
	ClawtBusyWatch *self
){
	self->snapshot = g_array_new(FALSE, TRUE, sizeof(ProcEntry));
	self->baselines = g_hash_table_new_full(g_str_hash, g_str_equal,
		g_free, g_free);
}
