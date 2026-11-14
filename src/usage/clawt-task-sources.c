/*
 * clawt-task-sources.c - Which routine or trigger run a task belongs to
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The usage ledger charges every turn of a routine's task tree to the
 * routine.  The daemon learns which run a task belongs to when the run
 * creates it, and held that in a hash table -- so a run that spanned a
 * restart lost the answer, and its remaining turns were tagged
 * `task:<id>`: still counted, charged to nobody anybody would look for.
 * Tasks themselves live in memory by design, but the turns about one do
 * not: a mailbox item carries its task id across a restart and is
 * delivered by the next daemon.  So the mapping is kept in a file of its
 * own, written when it changes.
 *
 * The file is one line per entry, tab-separated: task id, origin kind
 * nick, routine or trigger id, unix seconds noted.  Plain text because
 * there are four fields and nothing else reads it.
 */

#include "clawtilla.h"
#include "usage/clawt-task-sources.h"

#include <glib/gstdio.h>
#include <string.h>

typedef struct {
	ClawtUsageOrigin *origin;
	gint64            noted_at;
} Entry;

struct _ClawtTaskSources {
	GObject     parent_instance;

	gchar      *path;
	GHashTable *entries;
};

G_DEFINE_FINAL_TYPE(ClawtTaskSources, clawt_task_sources, G_TYPE_OBJECT)

static void
entry_free(
	gpointer data
){
	Entry *entry = data;

	if (entry == NULL)
		return;

	clawt_usage_origin_free(entry->origin);
	g_free(entry);
}

static Entry *
entry_new(
	ClawtUsageOriginKind  kind,
	const gchar          *id,
	gint64                noted_at
){
	Entry *entry = g_new0(Entry, 1);

	entry->origin = clawt_usage_origin_new(kind, id);
	entry->noted_at = noted_at;

	return entry;
}

/* Whether @text could be written as one field of one line. */
static gboolean
is_field(
	const gchar *text
){
	return text != NULL && *text != '\0' && strpbrk(text, "\t\r\n") == NULL;
}

/*
 * Reads @self->path into the table.  A line that does not parse is
 * skipped with a warning rather than failing the rest: one damaged entry
 * costs one attribution.
 */
static void
load(
	ClawtTaskSources *self
){
	g_autofree gchar *contents = NULL;
	g_autoptr(GError) error = NULL;
	g_auto(GStrv) lines = NULL;
	guint i;

	if (self->path == NULL || !g_file_test(self->path, G_FILE_TEST_EXISTS))
		return;

	if (!g_file_get_contents(self->path, &contents, NULL, &error)) {
		g_warning("task-sources: %s could not be read (%s); runs that "
			  "began before this start are charged to their task",
			  self->path, error->message);
		return;
	}

	lines = g_strsplit(contents, "\n", -1);

	for (i = 0; lines[i] != NULL; i++) {
		g_auto(GStrv) fields = NULL;
		ClawtUsageOriginKind kind;
		gint64 noted_at;

		if (lines[i][0] == '\0')
			continue;

		fields = g_strsplit(lines[i], "\t", 4);

		if (g_strv_length(fields) != 4 || !is_field(fields[0]) ||
			!is_field(fields[2])) {
			g_warning("task-sources: %s line %u is not an entry; skipped",
				  self->path, i + 1);
			continue;
		}

		kind = clawt_usage_origin_kind_from_nick(fields[1]);

		if (kind != CLAWT_USAGE_ORIGIN_ROUTINE &&
			kind != CLAWT_USAGE_ORIGIN_TRIGGER) {
			g_warning("task-sources: %s line %u names '%s', not a "
				  "routine or a trigger; skipped", self->path, i + 1,
				  fields[1]);
			continue;
		}

		noted_at = g_ascii_strtoll(fields[3], NULL, 10);

		g_hash_table_replace(self->entries, g_strdup(fields[0]),
				     entry_new(kind, fields[2], noted_at));
	}
}

/*
 * Writes the table out whole, atomically and 0600 -- the ids are
 * operator configuration rather than secrets, but nothing else needs to
 * read this file.
 */
static void
save(
	ClawtTaskSources *self
){
	g_autoptr(GString) out = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = NULL;
	GHashTableIter iter;
	gpointer key;
	gpointer value;

	if (self->path == NULL)
		return;

	out = g_string_new(NULL);
	g_hash_table_iter_init(&iter, self->entries);

	while (g_hash_table_iter_next(&iter, &key, &value)) {
		Entry *entry = value;

		g_string_append_printf(out, "%s\t%s\t%s\t%" G_GINT64_FORMAT "\n",
				       (const gchar *)key,
				       clawt_usage_origin_kind_nick(entry->origin->kind),
				       entry->origin->id, entry->noted_at);
	}

	dir = g_path_get_dirname(self->path);
	(void)g_mkdir_with_parents(dir, 0700);

	if (!g_file_set_contents_full(self->path, out->str, (gssize)out->len,
				      G_FILE_SET_CONTENTS_CONSISTENT, 0600,
				      &error))
		g_warning("task-sources: %s could not be written (%s); a run "
			  "spanning the next restart will be charged to its task",
			  self->path, error->message);
}

static void
clawt_task_sources_finalize(
	GObject *object
){
	ClawtTaskSources *self = CLAWT_TASK_SOURCES(object);

	g_clear_pointer(&self->entries, g_hash_table_unref);
	g_clear_pointer(&self->path, g_free);

	G_OBJECT_CLASS(clawt_task_sources_parent_class)->finalize(object);
}

static void
clawt_task_sources_class_init(
	ClawtTaskSourcesClass *klass
){
	G_OBJECT_CLASS(klass)->finalize = clawt_task_sources_finalize;
}

static void
clawt_task_sources_init(
	ClawtTaskSources *self
){
	self->entries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
					      entry_free);
}

ClawtTaskSources *
clawt_task_sources_new(
	const gchar *path
){
	ClawtTaskSources *self = g_object_new(CLAWT_TYPE_TASK_SOURCES, NULL);

	self->path = g_strdup(path);
	load(self);

	return self;
}

gboolean
clawt_task_sources_note(
	ClawtTaskSources     *self,
	const gchar          *task_id,
	ClawtUsageOriginKind  kind,
	const gchar          *id
){
	Entry *existing;

	g_return_val_if_fail(CLAWT_IS_TASK_SOURCES(self), FALSE);

	if (!is_field(task_id) || !is_field(id))
		return FALSE;

	if (kind != CLAWT_USAGE_ORIGIN_ROUTINE &&
		kind != CLAWT_USAGE_ORIGIN_TRIGGER)
		return FALSE;

	/* Nothing to write when it says what the table already does. */
	existing = g_hash_table_lookup(self->entries, task_id);
	if (existing != NULL && existing->origin->kind == kind &&
		g_strcmp0(existing->origin->id, id) == 0)
		return TRUE;

	g_hash_table_replace(self->entries, g_strdup(task_id),
			     entry_new(kind, id, g_get_real_time() / G_USEC_PER_SEC));
	save(self);

	return TRUE;
}

const ClawtUsageOrigin *
clawt_task_sources_lookup(
	ClawtTaskSources *self,
	const gchar      *task_id
){
	Entry *entry;

	g_return_val_if_fail(CLAWT_IS_TASK_SOURCES(self), NULL);

	if (task_id == NULL)
		return NULL;

	entry = g_hash_table_lookup(self->entries, task_id);

	return (entry != NULL) ? entry->origin : NULL;
}

guint
clawt_task_sources_get_count(
	ClawtTaskSources *self
){
	g_return_val_if_fail(CLAWT_IS_TASK_SOURCES(self), 0);

	return g_hash_table_size(self->entries);
}

guint
clawt_task_sources_prune(
	ClawtTaskSources         *self,
	ClawtTaskSourcesLiveFunc  is_live,
	gpointer                  user_data,
	gint64                    now
){
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	guint dropped = 0;

	g_return_val_if_fail(CLAWT_IS_TASK_SOURCES(self), 0);
	g_return_val_if_fail(is_live != NULL, 0);

	g_hash_table_iter_init(&iter, self->entries);

	while (g_hash_table_iter_next(&iter, &key, &value)) {
		Entry *entry = value;
		gboolean known = FALSE;
		gboolean live;

		live = is_live((const gchar *)key, &known, user_data);

		if (live)
			continue;

		/*
		 * Unknown is not finished.  After a restart no entry's task is
		 * known, and those are the entries this file is for.
		 */
		if (!known && now - entry->noted_at < CLAWT_TASK_SOURCES_MAX_AGE_SECONDS)
			continue;

		g_hash_table_iter_remove(&iter);
		dropped++;
	}

	if (dropped > 0)
		save(self);

	return dropped;
}
