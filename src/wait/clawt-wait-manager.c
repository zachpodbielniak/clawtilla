/*
 * clawt-wait-manager.c - The waits the fleet has set, and when to look
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "wait/clawt-wait-manager.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>

#define WAIT_FILE_SUFFIX ".json"

struct _ClawtWaitManager {
	GObject             parent_instance;

	gchar              *dir;

	/* id -> ClawtWait, pending ones only. */
	GHashTable         *waits;

	ClawtWaitProbeFunc  probe;
	gpointer            probe_data;
	GDestroyNotify      probe_notify;

	ClawtWaitEndFunc    end;
	gpointer            end_data;
	GDestroyNotify      end_notify;

	GSource            *tick;
};

G_DEFINE_FINAL_TYPE(ClawtWaitManager, clawt_wait_manager, G_TYPE_OBJECT)

enum {
	SIGNAL_CHANGED,
	N_SIGNALS
};

static guint signals[N_SIGNALS];

/* ── Disk ────────────────────────────────────────────────────────── */

static gchar *
path_for(
	ClawtWaitManager *self,
	const gchar      *id
){
	g_autofree gchar *name = g_strconcat(id, WAIT_FILE_SUFFIX, NULL);

	return g_build_filename(self->dir, name, NULL);
}

/*
 * One file per wait, written whole and renamed into place, 0600.
 *
 * A file each rather than one file for all of them, so that writing one
 * wait's last look cannot lose another's -- and so that a corrupt file
 * costs one wait rather than the fleet's.
 */
static gboolean
save(
	ClawtWaitManager  *self,
	ClawtWait         *wait,
	GError           **error
){
	g_autoptr(JsonNode) node = clawt_wait_to_json(wait);
	g_autofree gchar *text = json_to_string(node, TRUE);
	g_autofree gchar *path = path_for(self, wait->id);

	return g_file_set_contents_full(path, text, -1,
	                                G_FILE_SET_CONTENTS_CONSISTENT, 0600,
	                                error);
}

/* A failed write after the wait exists is a warning, not a lost wait. */
static void
save_or_warn(
	ClawtWaitManager *self,
	ClawtWait        *wait
){
	g_autoptr(GError) error = NULL;

	if (!save(self, wait, &error))
		g_warning("wait %s: could not write its record, so a restart "
		          "would forget its last look: %s", wait->id,
		          error->message);
}

static void
forget_file(
	ClawtWaitManager *self,
	const gchar      *id
){
	g_autofree gchar *path = path_for(self, id);

	if (g_unlink(path) != 0 && errno != ENOENT)
		g_warning("wait %s: could not remove %s: %s", id, path,
		          g_strerror(errno));
}

static void
load(
	ClawtWaitManager *self
){
	g_autoptr(GDir) dir = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *name;

	dir = g_dir_open(self->dir, 0, &error);

	if (dir == NULL) {
		g_warning("waits: could not read %s: %s", self->dir, error->message);
		return;
	}

	while ((name = g_dir_read_name(dir)) != NULL) {
		g_autofree gchar *path = NULL;
		g_autoptr(JsonParser) parser = NULL;
		g_autoptr(GError) read_error = NULL;
		g_autoptr(ClawtWait) wait = NULL;

		if (!g_str_has_suffix(name, WAIT_FILE_SUFFIX))
			continue;

		path = g_build_filename(self->dir, name, NULL);
		parser = json_parser_new();

		if (!json_parser_load_from_file(parser, path, &read_error)) {
			g_warning("waits: skipping %s, which is not JSON: %s", path,
			          read_error->message);
			continue;
		}

		wait = clawt_wait_from_json(json_parser_get_root(parser),
		                            &read_error);

		if (wait == NULL) {
			g_warning("waits: skipping %s: %s", path, read_error->message);
			continue;
		}

		/*
		 * Only pending waits are ever written, because an ended one is
		 * removed as it ends.  A file in another state is one whose
		 * removal failed after its notice went out, and reading it back
		 * as pending would deliver the notice twice.
		 */
		if (wait->state != CLAWT_WAIT_PENDING) {
			forget_file(self, wait->id);
			continue;
		}

		wait->in_flight = FALSE;

		/*
		 * The key is copied before the record is stolen: the order a
		 * call's arguments are evaluated in is unspecified, and a steal
		 * that ran first left the copy reading through NULL.
		 */
		{
			gchar *key = g_strdup(wait->id);

			g_hash_table_replace(self->waits, key, g_steal_pointer(&wait));
		}
	}
}

/* ── Ending ──────────────────────────────────────────────────────── */

static void
emit_changed(
	ClawtWaitManager *self,
	ClawtWait        *wait
){
	g_signal_emit(self, signals[SIGNAL_CHANGED], 0, wait->agent_id, wait->id);
}

/*
 * Takes a wait out of the table and off the disk, after telling the
 * agent.
 *
 * Told first and removed second.  A daemon that dies between the two
 * finds the record still pending and looks again -- which, for a
 * condition that held, delivers the notice a second time.  The other
 * order loses it, and a notice the agent gets twice is one it can see
 * is a repeat, while one it never gets is a wait it believes is still
 * running.
 */
static void
end_wait(
	ClawtWaitManager *self,
	ClawtWait        *wait,
	ClawtWaitState    state,
	gboolean          tell,
	gint64            now
){
	g_autoptr(ClawtWait) held = clawt_wait_ref(wait);

	if (held->state != CLAWT_WAIT_PENDING)
		return;

	held->state = state;
	held->ended_at = now;

	if (tell && self->end != NULL)
		self->end(self, held, self->end_data);

	g_hash_table_remove(self->waits, held->id);
	forget_file(self, held->id);
	emit_changed(self, held);
}

/* ── The manager ─────────────────────────────────────────────────── */

ClawtWaitManager *
clawt_wait_manager_new(
	const gchar  *dir,
	GError      **error
){
	g_autoptr(ClawtWaitManager) self = NULL;

	g_return_val_if_fail(dir != NULL, NULL);

	if (g_mkdir_with_parents(dir, 0700) != 0) {
		gint saved = errno;

		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved),
		            "could not create %s: %s", dir, g_strerror(saved));
		return NULL;
	}

	self = g_object_new(CLAWT_TYPE_WAIT_MANAGER, NULL);
	self->dir = g_strdup(dir);
	load(self);

	return g_steal_pointer(&self);
}

void
clawt_wait_manager_set_probe_func(
	ClawtWaitManager   *self,
	ClawtWaitProbeFunc  func,
	gpointer            user_data,
	GDestroyNotify      notify
){
	g_return_if_fail(CLAWT_IS_WAIT_MANAGER(self));

	if (self->probe_notify != NULL)
		self->probe_notify(self->probe_data);

	self->probe = func;
	self->probe_data = user_data;
	self->probe_notify = notify;
}

void
clawt_wait_manager_set_end_func(
	ClawtWaitManager *self,
	ClawtWaitEndFunc  func,
	gpointer          user_data,
	GDestroyNotify    notify
){
	g_return_if_fail(CLAWT_IS_WAIT_MANAGER(self));

	if (self->end_notify != NULL)
		self->end_notify(self->end_data);

	self->end = func;
	self->end_data = user_data;
	self->end_notify = notify;
}

/* `w-` and twelve hex digits: short enough to read out of a list. */
static gchar *
new_id(
	ClawtWaitManager *self
){
	for (;;) {
		g_autofree gchar *uuid = g_uuid_string_random();
		g_autoptr(GString) id = g_string_new("w-");
		const gchar *p;

		for (p = uuid; *p != '\0' && id->len < 14; p++) {
			if (*p != '-')
				g_string_append_c(id, *p);
		}

		if (!g_hash_table_contains(self->waits, id->str))
			return g_string_free(g_steal_pointer(&id), FALSE);
	}
}

gboolean
clawt_wait_manager_add(
	ClawtWaitManager  *self,
	ClawtWait         *wait,
	gint64             hours,
	guint              limit,
	gint64             now,
	GError           **error
){
	guint already;

	g_return_val_if_fail(CLAWT_IS_WAIT_MANAGER(self), FALSE);
	g_return_val_if_fail(wait != NULL, FALSE);
	g_return_val_if_fail(wait->agent_id != NULL, FALSE);
	g_return_val_if_fail(wait->room_id != NULL, FALSE);

	already = clawt_wait_manager_count(self, wait->agent_id);

	if (limit > 0 && already >= limit) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
		            "%s already has %u pending wait%s, the most it may have "
		            "(orchestration.max_waits_per_agent)", wait->agent_id,
		            already, already == 1 ? "" : "s");
		return FALSE;
	}

	g_clear_pointer(&wait->id, g_free);
	wait->id = new_id(self);
	wait->state = CLAWT_WAIT_PENDING;
	wait->created_at = now;
	wait->expires_at = now +
		clawt_wait_clamp_hours(hours, NULL) * 3600 * G_USEC_PER_SEC;
	wait->last_check_at = 0;
	wait->next_check_at = now +
		(gint64)clawt_wait_interval_seconds(wait->kind, 0) * G_USEC_PER_SEC;
	wait->ended_at = 0;
	wait->checks = 0;
	wait->failures = 0;
	wait->in_flight = FALSE;

	if (!save(self, wait, error))
		return FALSE;

	g_hash_table_replace(self->waits, g_strdup(wait->id),
	                     clawt_wait_ref(wait));
	emit_changed(self, wait);

	return TRUE;
}

ClawtWait *
clawt_wait_manager_get(
	ClawtWaitManager *self,
	const gchar      *id
){
	g_return_val_if_fail(CLAWT_IS_WAIT_MANAGER(self), NULL);

	if (id == NULL)
		return NULL;

	return g_hash_table_lookup(self->waits, id);
}

static gint
by_created(
	gconstpointer a,
	gconstpointer b
){
	const ClawtWait *left = *(ClawtWait *const *)a;
	const ClawtWait *right = *(ClawtWait *const *)b;

	if (left->created_at != right->created_at)
		return (left->created_at < right->created_at) ? -1 : 1;

	return g_strcmp0(left->id, right->id);
}

GPtrArray *
clawt_wait_manager_list(
	ClawtWaitManager *self,
	const gchar      *agent_id
){
	GPtrArray *out;
	GHashTableIter iter;
	gpointer value;

	g_return_val_if_fail(CLAWT_IS_WAIT_MANAGER(self), NULL);

	out = g_ptr_array_new_with_free_func((GDestroyNotify)clawt_wait_unref);
	g_hash_table_iter_init(&iter, self->waits);

	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		ClawtWait *wait = value;

		if (agent_id == NULL || g_strcmp0(wait->agent_id, agent_id) == 0)
			g_ptr_array_add(out, clawt_wait_ref(wait));
	}

	g_ptr_array_sort(out, by_created);

	return out;
}

guint
clawt_wait_manager_count(
	ClawtWaitManager *self,
	const gchar      *agent_id
){
	GHashTableIter iter;
	gpointer value;
	guint count = 0;

	g_return_val_if_fail(CLAWT_IS_WAIT_MANAGER(self), 0);

	g_hash_table_iter_init(&iter, self->waits);

	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		ClawtWait *wait = value;

		if (agent_id == NULL || g_strcmp0(wait->agent_id, agent_id) == 0)
			count++;
	}

	return count;
}

gboolean
clawt_wait_manager_cancel(
	ClawtWaitManager  *self,
	const gchar       *id,
	const gchar       *agent_id,
	gint64             now,
	GError           **error
){
	ClawtWait *wait;

	g_return_val_if_fail(CLAWT_IS_WAIT_MANAGER(self), FALSE);

	wait = clawt_wait_manager_get(self, id);

	/*
	 * Another agent's wait reads as no wait at all.  Saying "that is
	 * not yours" would tell an agent that an id it guessed exists.
	 */
	if (wait == NULL ||
	    (agent_id != NULL && g_strcmp0(wait->agent_id, agent_id) != 0)) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
		            "there is no pending wait %s", id != NULL ? id : "");
		return FALSE;
	}

	end_wait(self, wait, CLAWT_WAIT_CANCELLED, agent_id == NULL, now);

	return TRUE;
}

guint
clawt_wait_manager_drop_agent(
	ClawtWaitManager *self,
	const gchar      *agent_id
){
	g_autoptr(GPtrArray) theirs = NULL;
	guint i;

	g_return_val_if_fail(CLAWT_IS_WAIT_MANAGER(self), 0);

	theirs = clawt_wait_manager_list(self, agent_id);

	for (i = 0; i < theirs->len; i++)
		end_wait(self, g_ptr_array_index(theirs, i), CLAWT_WAIT_FAILED,
		         FALSE, g_get_real_time());

	return theirs->len;
}

void
clawt_wait_manager_tick(
	ClawtWaitManager *self,
	gint64            now
){
	g_autoptr(GPtrArray) all = NULL;
	guint i;

	g_return_if_fail(CLAWT_IS_WAIT_MANAGER(self));

	/*
	 * A snapshot, because a probe may report from inside this loop and
	 * a report may end a wait -- which removes it from the table being
	 * walked.
	 */
	all = clawt_wait_manager_list(self, NULL);

	for (i = 0; i < all->len; i++) {
		ClawtWait *wait = g_ptr_array_index(all, i);

		if (wait->state != CLAWT_WAIT_PENDING)
			continue;

		/*
		 * Expiry first, and even with a look out: the probe's late
		 * answer is ignored because the wait is no longer pending, which
		 * is the right outcome for a condition that took longer than
		 * the agent said it was worth waiting for.
		 */
		if (now >= wait->expires_at) {
			end_wait(self, wait, CLAWT_WAIT_EXPIRED, TRUE, now);
			continue;
		}

		if (wait->in_flight || now < wait->next_check_at ||
		    self->probe == NULL)
			continue;

		wait->in_flight = TRUE;
		self->probe(self, wait, self->probe_data);
	}
}

void
clawt_wait_manager_report(
	ClawtWaitManager *self,
	ClawtWait        *wait,
	ClawtWaitVerdict  verdict,
	const gchar      *seen,
	const gchar      *quoted,
	gint64            now
){
	guint gap;
	guint failures_before;
	gboolean news;

	g_return_if_fail(CLAWT_IS_WAIT_MANAGER(self));
	g_return_if_fail(wait != NULL);

	wait->in_flight = FALSE;

	/*
	 * Ended while the look was out -- cancelled, expired, its agent
	 * removed.  The table is the authority on that, not the record's
	 * own state, because the probe may hold a record the table has
	 * already let go of.
	 */
	if (wait->state != CLAWT_WAIT_PENDING ||
	    g_hash_table_lookup(self->waits, wait->id) != wait)
		return;

	wait->checks++;
	wait->last_check_at = now;
	failures_before = wait->failures;

	/*
	 * Whether a person looking at the list would see anything new.  A
	 * look that saw what the last one saw is written to disk -- the
	 * schedule moved -- and announced to nobody: a file wait is looked
	 * at every fifteen seconds, and an event each time would fill the
	 * event log with "still not there" and redraw every client's agent
	 * view four times a minute.
	 */
	news = (g_strcmp0(seen, wait->last_result) != 0);

	if (seen != NULL) {
		g_free(wait->last_result);
		wait->last_result = g_strdup(seen);
	}

	switch (verdict) {
	case CLAWT_WAIT_MET:
		g_free(wait->quoted);
		wait->quoted = g_strdup(quoted);
		end_wait(self, wait, CLAWT_WAIT_SATISFIED, TRUE, now);
		return;

	case CLAWT_WAIT_IMPOSSIBLE:
		end_wait(self, wait, CLAWT_WAIT_FAILED, TRUE, now);
		return;

	case CLAWT_WAIT_UNANSWERED:
		wait->failures++;
		break;

	case CLAWT_WAIT_NOT_YET:
		wait->failures = 0;
		break;
	}

	gap = clawt_wait_interval_seconds(wait->kind, wait->failures);
	wait->next_check_at = now + (gint64)gap * G_USEC_PER_SEC;

	save_or_warn(self, wait);

	if (news || (failures_before == 0) != (wait->failures == 0))
		emit_changed(self, wait);
}

static gboolean
on_tick(
	gpointer user_data
){
	ClawtWaitManager *self = user_data;

	clawt_wait_manager_tick(self, g_get_real_time());

	return G_SOURCE_CONTINUE;
}

void
clawt_wait_manager_start(
	ClawtWaitManager *self,
	GMainContext     *context
){
	g_return_if_fail(CLAWT_IS_WAIT_MANAGER(self));

	if (self->tick != NULL)
		return;

	/*
	 * Named here, where the source attaches.  clawt_timeout_add_seconds()
	 * takes the thread-default, and the caller of this is reached from
	 * daemon start -- which an embedding program may run from a context
	 * that is not the daemon's.
	 */
	if (context != NULL)
		g_main_context_push_thread_default(context);

	self->tick = clawt_timeout_add_seconds(CLAWT_WAIT_TICK_SECONDS, on_tick,
	                                       self);

	if (context != NULL)
		g_main_context_pop_thread_default(context);
}

void
clawt_wait_manager_stop(
	ClawtWaitManager *self
){
	g_return_if_fail(CLAWT_IS_WAIT_MANAGER(self));

	if (self->tick != NULL) {
		g_source_destroy(self->tick);
		g_clear_pointer(&self->tick, g_source_unref);
	}
}

static void
clawt_wait_manager_finalize(
	GObject *object
){
	ClawtWaitManager *self = CLAWT_WAIT_MANAGER(object);

	clawt_wait_manager_stop(self);
	clawt_wait_manager_set_probe_func(self, NULL, NULL, NULL);
	clawt_wait_manager_set_end_func(self, NULL, NULL, NULL);
	g_clear_pointer(&self->waits, g_hash_table_unref);
	g_clear_pointer(&self->dir, g_free);

	G_OBJECT_CLASS(clawt_wait_manager_parent_class)->finalize(object);
}

static void
clawt_wait_manager_class_init(
	ClawtWaitManagerClass *klass
){
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->finalize = clawt_wait_manager_finalize;

	/**
	 * ClawtWaitManager::changed:
	 * @self: the manager
	 * @agent_id: whose wait
	 * @wait_id: which wait
	 *
	 * A wait was added, looked at, or ended.
	 */
	signals[SIGNAL_CHANGED] = g_signal_new(
		"changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
		NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);
}

static void
clawt_wait_manager_init(
	ClawtWaitManager *self
){
	self->waits = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                    (GDestroyNotify)clawt_wait_unref);
}
