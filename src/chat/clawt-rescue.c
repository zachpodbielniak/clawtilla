/*
 * clawt-rescue.c - A turn that has shown nothing at all, and what to do
 * about it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "chat/clawt-rescue.h"

#include <libreclaw.h>

/* One silent turn. */
typedef struct {
	gint64   began;
	gboolean offered;
} Silent;

struct _ClawtRescueWatch {
	GObject parent_instance;

	GHashTable *turns;   /* "agent\037room" -> Silent* */
};

G_DEFINE_FINAL_TYPE(ClawtRescueWatch, clawt_rescue_watch, G_TYPE_OBJECT)

static gchar *
key_for(
	const gchar *agent_id,
	const gchar *room_id
){
	return g_strdup_printf("%s\037%s", agent_id, room_id != NULL ? room_id
		: "");
}

ClawtRescueWatch *
clawt_rescue_watch_new(
	void
){
	return g_object_new(CLAWT_TYPE_RESCUE_WATCH, NULL);
}

void
clawt_rescue_watch_begin(
	ClawtRescueWatch *self,
	const gchar      *agent_id,
	const gchar      *room_id,
	gint64            now_usec
){
	Silent *silent;

	g_return_if_fail(CLAWT_IS_RESCUE_WATCH(self));

	if (agent_id == NULL)
		return;

	silent = g_new0(Silent, 1);
	silent->began = now_usec;
	g_hash_table_replace(self->turns, key_for(agent_id, room_id), silent);
}

gboolean
clawt_rescue_watch_note_life(
	ClawtRescueWatch *self,
	const gchar      *agent_id,
	const gchar      *room_id
){
	g_autofree gchar *prefix = NULL;
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	gboolean was_offered = FALSE;

	g_return_val_if_fail(CLAWT_IS_RESCUE_WATCH(self), FALSE);

	if (agent_id == NULL)
		return FALSE;

	if (room_id != NULL) {
		g_autofree gchar *one = key_for(agent_id, room_id);
		Silent *silent = g_hash_table_lookup(self->turns, one);

		if (silent == NULL)
			return FALSE;

		was_offered = silent->offered;
		g_hash_table_remove(self->turns, one);
		return was_offered;
	}

	/* Every room of the agent: a settle, or a stop. */
	prefix = g_strdup_printf("%s\037", agent_id);
	g_hash_table_iter_init(&iter, self->turns);

	while (g_hash_table_iter_next(&iter, &key, &value)) {
		if (!g_str_has_prefix(key, prefix))
			continue;

		was_offered = was_offered || ((Silent *)value)->offered;
		g_hash_table_iter_remove(&iter);
	}

	return was_offered;
}

GPtrArray *
clawt_rescue_watch_collect_due(
	ClawtRescueWatch *self,
	gint64            now_usec,
	ClawtRescueAllowanceFunc  seconds_for,
	gpointer                  user_data
){
	GPtrArray *due;
	GHashTableIter iter;
	gpointer key;
	gpointer value;

	g_return_val_if_fail(CLAWT_IS_RESCUE_WATCH(self), NULL);
	g_return_val_if_fail(seconds_for != NULL, NULL);

	due = g_ptr_array_new_with_free_func(g_free);
	g_hash_table_iter_init(&iter, self->turns);

	while (g_hash_table_iter_next(&iter, &key, &value)) {
		Silent *silent = value;
		g_autofree gchar *agent_id = NULL;
		g_autofree gchar *room_id = NULL;
		guint allowance;

		if (silent->offered)
			continue;

		clawt_rescue_key_split(key, &agent_id, &room_id);
		allowance = seconds_for(agent_id, user_data);

		if (allowance == 0)
			continue;

		if (now_usec - silent->began < (gint64)allowance * G_USEC_PER_SEC)
			continue;

		silent->offered = TRUE;
		g_ptr_array_add(due, g_strdup(key));
	}

	return due;
}

GPtrArray *
clawt_rescue_watch_offered(
	ClawtRescueWatch *self,
	const gchar      *agent_id
){
	g_autofree gchar *prefix = NULL;
	GPtrArray *out;
	GHashTableIter iter;
	gpointer key;
	gpointer value;

	g_return_val_if_fail(CLAWT_IS_RESCUE_WATCH(self), NULL);

	out = g_ptr_array_new_with_free_func(g_free);
	prefix = (agent_id != NULL) ? g_strdup_printf("%s\037", agent_id) : NULL;
	g_hash_table_iter_init(&iter, self->turns);

	while (g_hash_table_iter_next(&iter, &key, &value)) {
		if (!((Silent *)value)->offered)
			continue;

		if (prefix != NULL && !g_str_has_prefix(key, prefix))
			continue;

		g_ptr_array_add(out, g_strdup(key));
	}

	return out;
}

gint64
clawt_rescue_watch_silent_for(
	ClawtRescueWatch *self,
	const gchar      *agent_id,
	const gchar      *room_id,
	gint64            now_usec
){
	g_autofree gchar *key = NULL;
	Silent *silent;

	g_return_val_if_fail(CLAWT_IS_RESCUE_WATCH(self), -1);

	key = key_for(agent_id, room_id);
	silent = g_hash_table_lookup(self->turns, key);

	return (silent != NULL)
		? MAX(now_usec - silent->began, 0) / G_USEC_PER_SEC : -1;
}

void
clawt_rescue_key_split(
	const gchar  *key,
	gchar       **agent_id,
	gchar       **room_id
){
	const gchar *mark;

	g_return_if_fail(agent_id != NULL);
	g_return_if_fail(room_id != NULL);

	mark = (key != NULL) ? strchr(key, '\037') : NULL;

	if (mark == NULL) {
		*agent_id = g_strdup(key);
		*room_id = NULL;
		return;
	}

	*agent_id = g_strndup(key, (gsize)(mark - key));
	*room_id = g_strdup(mark + 1);
}

/* ── Which models one turn can move to ──────────────────────────────── */

GPtrArray *
clawt_rescue_switch_choices(
	const gchar  *provider,
	const gchar  *account,
	GPtrArray    *fallbacks,
	GPtrArray   **refused
){
	GPtrArray *choices = g_ptr_array_new_with_free_func(g_free);
	const gchar *own;
	guint i;

	if (refused != NULL)
		*refused = g_ptr_array_new_with_free_func(g_free);

	/*
	 * Through libreclaw's normaliser, which is what decides which CLI an
	 * agent actually runs: "claude" and "claude-code" are one CLI, and
	 * comparing the spellings would refuse a choice that works.
	 */
	own = (provider != NULL && *provider != '\0')
		? lc_provider_type_normalize(provider, "agent") : NULL;

	for (i = 0; fallbacks != NULL && i < fallbacks->len; i++) {
		ClawtModelFallback *entry = g_ptr_array_index(fallbacks, i);
		const gchar *theirs = (entry->provider != NULL &&
			*entry->provider != '\0')
			? lc_provider_type_normalize(entry->provider, "fallback") : NULL;
		gboolean same_cli = own != NULL && g_strcmp0(own, theirs) == 0;
		gboolean same_login = g_strcmp0(
			(entry->account != NULL && *entry->account != '\0')
				? entry->account : NULL,
			(account != NULL && *account != '\0') ? account : NULL) == 0;
		guint j;
		gboolean seen = FALSE;

		if (!same_cli || !same_login) {
			if (refused != NULL) {
				g_autofree gchar *label = clawt_model_fallback_label(entry);

				g_ptr_array_add(*refused, g_strdup_printf(
					"%s runs on %s, and one turn can only change the "
					"model on the CLI and login it is already running "
					"(%s); libreclaw falls over to it by itself at a "
					"limit", label, !same_cli ? "another CLI"
						: "another login", own != NULL ? own : "none"));
			}
			continue;
		}

		if (entry->model == NULL || *entry->model == '\0')
			continue;

		for (j = 0; j < choices->len; j++)
			if (g_strcmp0(g_ptr_array_index(choices, j), entry->model) == 0)
				seen = TRUE;

		if (!seen)
			g_ptr_array_add(choices, g_strdup(entry->model));
	}

	return choices;
}

gchar *
clawt_rescue_offer_text(
	const gchar *agent_name,
	gint64       seconds
){
	return g_strdup_printf("%s has not replied or shown any activity for "
		"%" G_GINT64_FORMAT " seconds. It may still be starting, or "
		"writing an answer it has not sent yet -- or it may be stuck.",
		agent_name != NULL ? agent_name : "The agent", MAX(seconds, 0));
}

gchar *
clawt_rescue_switch_label(
	const gchar *model
){
	return g_strdup_printf("Try %s instead", model != NULL ? model : "?");
}

static void
clawt_rescue_watch_finalize(
	GObject *object
){
	ClawtRescueWatch *self = CLAWT_RESCUE_WATCH(object);

	g_clear_pointer(&self->turns, g_hash_table_unref);

	G_OBJECT_CLASS(clawt_rescue_watch_parent_class)->finalize(object);
}

static void
clawt_rescue_watch_class_init(
	ClawtRescueWatchClass *klass
){
	G_OBJECT_CLASS(klass)->finalize = clawt_rescue_watch_finalize;
}

static void
clawt_rescue_watch_init(
	ClawtRescueWatch *self
){
	self->turns = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
		g_free);
}
