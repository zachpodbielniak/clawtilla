/*
 * clawt-secret-vault.c - One-use references to secrets an agent never sees
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Minted on the daemon's context (clawtilla_secret_ref) and redeemed on
 * a browser worker (clawtilla_browser_fill_secret), so the table is
 * behind a mutex.
 */

#include "clawtilla.h"

#include <string.h>

/* Spent references are remembered this long, to say "already used". */
#define SPENT_MEMORY_SECONDS (CLAWT_SECRET_REF_TTL_SECONDS * 6)

/* Bounds both tables: an agent minting in a loop must not grow them. */
#define MAX_LIVE_REFS (256)

typedef struct {
	gchar  *agent_id;
	gchar  *room_id;   /* "" for none */
	gchar  *name;
	gint64  expires_at;
} RefEntry;

struct _ClawtSecretVault {
	GObject     parent_instance;
	GMutex      lock;
	GHashTable *live;    /* ref -> RefEntry */
	GHashTable *spent;   /* ref -> GINT64 (when it was spent), as a boxed gint64 */
};

G_DEFINE_FINAL_TYPE(ClawtSecretVault, clawt_secret_vault, G_TYPE_OBJECT)

static void
ref_entry_free(
	gpointer data
){
	RefEntry *entry = data;

	g_free(entry->agent_id);
	g_free(entry->room_id);
	g_free(entry->name);
	g_free(entry);
}

static void
clawt_secret_vault_finalize(
	GObject *object
){
	ClawtSecretVault *self = CLAWT_SECRET_VAULT(object);

	g_hash_table_unref(self->live);
	g_hash_table_unref(self->spent);
	g_mutex_clear(&self->lock);

	G_OBJECT_CLASS(clawt_secret_vault_parent_class)->finalize(object);
}

static void
clawt_secret_vault_class_init(
	ClawtSecretVaultClass *klass
){
	G_OBJECT_CLASS(klass)->finalize = clawt_secret_vault_finalize;
}

static void
clawt_secret_vault_init(
	ClawtSecretVault *self
){
	g_mutex_init(&self->lock);
	self->live = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, ref_entry_free);
	self->spent = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}

ClawtSecretVault *
clawt_secret_vault_new(void)
{
	return (ClawtSecretVault *)g_object_new(CLAWT_TYPE_SECRET_VAULT, NULL);
}

/* Drops expired references and long-spent ones.  Called with the lock held. */
static void
prune_locked(
	ClawtSecretVault *self,
	gint64            now
){
	GHashTableIter iter;
	gpointer key;
	gpointer value;

	g_hash_table_iter_init(&iter, self->live);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		if (((RefEntry *)value)->expires_at <= now)
			g_hash_table_iter_remove(&iter);
	}
	g_hash_table_iter_init(&iter, self->spent);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		if (*(gint64 *)value + SPENT_MEMORY_SECONDS <= now)
			g_hash_table_iter_remove(&iter);
	}
}

/* Moves @ref from the live table to the spent one.  Lock held. */
static void
spend_locked(
	ClawtSecretVault *self,
	const gchar      *ref,
	gint64            now
){
	gint64 *when = g_new(gint64, 1);

	*when = now;
	g_hash_table_remove(self->live, ref);
	g_hash_table_replace(self->spent, g_strdup(ref), when);
}

gchar *
clawt_secret_vault_mint(
	ClawtSecretVault  *self,
	const gchar       *agent_id,
	const gchar       *room_id,
	const gchar       *name,
	gint64             now,
	GError           **error
){
	g_autofree gchar *token = NULL;
	gchar *ref;
	RefEntry *entry;

	g_return_val_if_fail(CLAWT_IS_SECRET_VAULT(self), NULL);
	g_return_val_if_fail(agent_id != NULL, NULL);
	g_return_val_if_fail(name != NULL, NULL);

	/* From the kernel's pool, never g_random_*: a guessable reference is
	 * a reference somebody else can spend. */
	token = clawt_generate_token(error);
	if (token == NULL)
		return NULL;
	token[32] = '\0';
	ref = g_strconcat(CLAWT_SECRET_REF_PREFIX, token, NULL);

	entry = g_new0(RefEntry, 1);
	entry->agent_id = g_strdup(agent_id);
	entry->room_id = g_strdup(room_id != NULL ? room_id : "");
	entry->name = g_strdup(name);
	entry->expires_at = now + CLAWT_SECRET_REF_TTL_SECONDS;

	g_mutex_lock(&self->lock);
	prune_locked(self, now);
	if (g_hash_table_size(self->live) >= MAX_LIVE_REFS) {
		g_mutex_unlock(&self->lock);
		ref_entry_free(entry);
		g_free(ref);
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
		                    "too many unused secret references are outstanding; "
		                    "use the ones you have, or wait for them to expire");
		return NULL;
	}
	g_hash_table_replace(self->live, g_strdup(ref), entry);
	g_mutex_unlock(&self->lock);

	return ref;
}

gchar *
clawt_secret_vault_redeem(
	ClawtSecretVault  *self,
	const gchar       *ref,
	const gchar       *agent_id,
	const gchar       *room_id,
	gint64             now,
	GError           **error
){
	RefEntry *entry;
	gchar *name = NULL;

	g_return_val_if_fail(CLAWT_IS_SECRET_VAULT(self), NULL);

	if (ref == NULL || !g_str_has_prefix(ref, CLAWT_SECRET_REF_PREFIX)) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		                    "that is not a secret reference: ask for one with "
		                    "clawtilla_secret_ref, and pass what it returns");
		return NULL;
	}

	g_mutex_lock(&self->lock);
	entry = g_hash_table_lookup(self->live, ref);

	/* 1. Unknown, or spent already. */
	if (entry == NULL) {
		gboolean spent = g_hash_table_contains(self->spent, ref);

		g_mutex_unlock(&self->lock);
		g_set_error_literal(error, CLAWT_ERROR,
		                    spent ? CLAWT_ERROR_PERMISSION_DENIED : CLAWT_ERROR_NOT_FOUND,
		                    spent
		                    ? "that secret reference was already used: each one works "
		                      "once. Ask for another with clawtilla_secret_ref"
		                    : "that secret reference is not one this daemon holds: it "
		                      "is mistyped, or the daemon restarted. Ask for another "
		                      "with clawtilla_secret_ref");
		return NULL;
	}

	/* 2. Expired, or given to somebody else: refused and spent. */
	if (entry->expires_at <= now) {
		spend_locked(self, ref, now);
		g_mutex_unlock(&self->lock);
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED,
		                    "that secret reference expired. Ask for another with "
		                    "clawtilla_secret_ref just before you fill");
		return NULL;
	}
	if (g_strcmp0(entry->agent_id, agent_id) != 0 ||
	    g_strcmp0(entry->room_id, room_id != NULL ? room_id : "") != 0) {
		spend_locked(self, ref, now);
		g_mutex_unlock(&self->lock);
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED,
		                    "that secret reference was given to another agent or "
		                    "another conversation, and is now spent. Ask for your own "
		                    "with clawtilla_secret_ref, here");
		return NULL;
	}

	/* 3. Good: spent by this redemption, whatever the fill then does. */
	name = g_strdup(entry->name);
	spend_locked(self, ref, now);
	g_mutex_unlock(&self->lock);
	return name;
}

guint
clawt_secret_vault_count_live(
	ClawtSecretVault *self,
	gint64            now
){
	guint count;

	g_return_val_if_fail(CLAWT_IS_SECRET_VAULT(self), 0);

	g_mutex_lock(&self->lock);
	prune_locked(self, now);
	count = g_hash_table_size(self->live);
	g_mutex_unlock(&self->lock);
	return count;
}

void
clawt_secret_vault_forget_agent(
	ClawtSecretVault *self,
	const gchar      *agent_id
){
	GHashTableIter iter;
	gpointer key;
	gpointer value;

	g_return_if_fail(CLAWT_IS_SECRET_VAULT(self));

	g_mutex_lock(&self->lock);
	g_hash_table_iter_init(&iter, self->live);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		if (g_strcmp0(((RefEntry *)value)->agent_id, agent_id) == 0)
			g_hash_table_iter_remove(&iter);
	}
	g_mutex_unlock(&self->lock);
}
