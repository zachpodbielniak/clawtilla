/*
 * clawt-fed-store.c - federation.db
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "federation/clawt-fed-store.h"

#include <glib/gstdio.h>
#include <sqlite3.h>
#include <string.h>

struct _ClawtFedStore {
    GObject  parent_instance;
    sqlite3 *db;
    gchar   *path;
};

G_DEFINE_FINAL_TYPE(ClawtFedStore, clawt_fed_store, G_TYPE_OBJECT)

static const gchar SCHEMA_SQL[] =
    "CREATE TABLE IF NOT EXISTS audit ("
    "  time INTEGER NOT NULL, direction TEXT NOT NULL, peer TEXT NOT NULL,"
    "  kind TEXT NOT NULL, outcome TEXT NOT NULL, detail TEXT NOT NULL);"
    "CREATE INDEX IF NOT EXISTS audit_time ON audit(time);"
    "CREATE INDEX IF NOT EXISTS audit_peer ON audit(peer, time);"
    "CREATE TABLE IF NOT EXISTS nonces ("
    "  fleet TEXT NOT NULL, nonce TEXT NOT NULL, time INTEGER NOT NULL,"
    "  PRIMARY KEY (fleet, nonce));"
    "CREATE INDEX IF NOT EXISTS nonces_time ON nonces(time);"
    "CREATE TABLE IF NOT EXISTS msg_outbox ("
    "  id TEXT PRIMARY KEY, alias TEXT NOT NULL, kind TEXT NOT NULL,"
    "  body TEXT NOT NULL, room TEXT, queued INTEGER NOT NULL);"
    "CREATE TABLE IF NOT EXISTS deliveries ("
    "  message TEXT PRIMARY KEY, room TEXT NOT NULL, state TEXT NOT NULL,"
    "  detail TEXT, time INTEGER NOT NULL);"
    "CREATE INDEX IF NOT EXISTS deliveries_room ON deliveries(room);"
    "CREATE TABLE IF NOT EXISTS kv ("
    "  tab TEXT NOT NULL, key TEXT NOT NULL, value TEXT NOT NULL,"
    "  PRIMARY KEY (tab, key));";

static void
clawt_fed_store_finalize(
    GObject *object
){
    ClawtFedStore *self = CLAWT_FED_STORE(object);

    if (self->db != NULL)
        sqlite3_close_v2(self->db);
    g_free(self->path);
    G_OBJECT_CLASS(clawt_fed_store_parent_class)->finalize(object);
}

static void
clawt_fed_store_class_init(
    ClawtFedStoreClass *klass
){
    G_OBJECT_CLASS(klass)->finalize = clawt_fed_store_finalize;
}

static void
clawt_fed_store_init(
    ClawtFedStore *self
){
    (void)self;
}

ClawtFedStore *
clawt_fed_store_open(
    const gchar  *path,
    GError      **error
){
    g_autoptr(ClawtFedStore) self = NULL;
    g_autofree gchar *dir = NULL;

    g_return_val_if_fail(path != NULL, NULL);
    dir = g_path_get_dirname(path);
    if (!clawt_ensure_dir(dir, 0700, error))
        return NULL;
    self = g_object_new(CLAWT_TYPE_FED_STORE, NULL);
    self->path = g_strdup(path);
    if (sqlite3_open(path, &self->db) != SQLITE_OK) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED, "opening %s: %s", path,
                    self->db != NULL ? sqlite3_errmsg(self->db) : "out of memory");
        return NULL;
    }
    g_chmod(path, 0600);
    sqlite3_busy_timeout(self->db, 2000);
    sqlite3_exec(self->db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(self->db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
    if (sqlite3_exec(self->db, SCHEMA_SQL, NULL, NULL, NULL) != SQLITE_OK) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED, "creating %s's tables: %s", path,
                    sqlite3_errmsg(self->db));
        return NULL;
    }
    return g_steal_pointer(&self);
}

/* One statement, run; a failure is a warning: federation's record is not worth a crash. */
static sqlite3_stmt *
prepare(
    ClawtFedStore *self,
    const gchar   *sql
){
    sqlite3_stmt *stmt = NULL;

    if (sqlite3_prepare_v2(self->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        g_warning("federation.db: %s", sqlite3_errmsg(self->db));
        return NULL;
    }
    return stmt;
}

static void
finish(
    ClawtFedStore *self,
    sqlite3_stmt  *stmt
){
    if (stmt == NULL)
        return;
    if (sqlite3_step(stmt) != SQLITE_DONE)
        g_warning("federation.db: %s", sqlite3_errmsg(self->db));
    sqlite3_finalize(stmt);
}

static const gchar *
str_or(
    JsonObject  *o,
    const gchar *member,
    const gchar *fallback
){
    JsonNode *n = o != NULL ? json_object_get_member(o, member) : NULL;

    return n != NULL && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING
        ? json_node_get_string(n) : fallback;
}

static gchar *
text_of(
    JsonObject *o
){
    g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);

    json_node_set_object(node, o);
    return json_to_string(node, FALSE);
}

static JsonObject *
object_of(
    const gchar *text
){
    g_autoptr(JsonParser) parser = json_parser_new();
    JsonNode *root;

    if (text == NULL || !json_parser_load_from_data(parser, text, -1, NULL))
        return NULL;
    root = json_parser_get_root(parser);
    return root != NULL && JSON_NODE_HOLDS_OBJECT(root)
        ? json_object_ref(json_node_get_object(root)) : NULL;
}

void
clawt_fed_store_audit_add(
    ClawtFedStore *self,
    JsonObject    *entry
){
    sqlite3_stmt *stmt;

    g_return_if_fail(CLAWT_IS_FED_STORE(self));
    stmt = prepare(self, "INSERT INTO audit(time, direction, peer, kind, outcome, detail) "
                         "VALUES (?, ?, ?, ?, ?, ?)");
    if (stmt == NULL)
        return;
    sqlite3_bind_int64(stmt, 1, json_object_get_int_member_with_default(entry, "time", 0));
    sqlite3_bind_text(stmt, 2, str_or(entry, "direction", ""), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, str_or(entry, "peer", ""), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, str_or(entry, "kind", ""), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, str_or(entry, "outcome", ""), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, str_or(entry, "detail", ""), -1, SQLITE_TRANSIENT);
    finish(self, stmt);
}

JsonArray *
clawt_fed_store_audit_list(
    ClawtFedStore *self,
    const gchar   *peer,
    guint          limit
){
    JsonArray *out = json_array_new();
    sqlite3_stmt *stmt;

    g_return_val_if_fail(CLAWT_IS_FED_STORE(self), out);
    stmt = prepare(self, peer != NULL
        ? "SELECT time, direction, peer, kind, outcome, detail FROM audit WHERE peer = ? "
          "ORDER BY time DESC, rowid DESC LIMIT ?"
        : "SELECT time, direction, peer, kind, outcome, detail FROM audit "
          "ORDER BY time DESC, rowid DESC LIMIT ?");
    if (stmt == NULL)
        return out;
    if (peer != NULL) {
        sqlite3_bind_text(stmt, 1, peer, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, (gint)limit);
    } else {
        sqlite3_bind_int(stmt, 1, (gint)limit);
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        JsonObject *e = json_object_new();

        json_object_set_int_member(e, "time", sqlite3_column_int64(stmt, 0));
        json_object_set_string_member(e, "direction", (const gchar *)sqlite3_column_text(stmt, 1));
        json_object_set_string_member(e, "peer", (const gchar *)sqlite3_column_text(stmt, 2));
        json_object_set_string_member(e, "kind", (const gchar *)sqlite3_column_text(stmt, 3));
        json_object_set_string_member(e, "outcome", (const gchar *)sqlite3_column_text(stmt, 4));
        json_object_set_string_member(e, "detail", (const gchar *)sqlite3_column_text(stmt, 5));
        json_array_add_object_element(out, e);
    }
    sqlite3_finalize(stmt);
    return out;
}

void
clawt_fed_store_audit_prune(
    ClawtFedStore *self,
    gint64         before
){
    sqlite3_stmt *stmt;

    g_return_if_fail(CLAWT_IS_FED_STORE(self));
    stmt = prepare(self, "DELETE FROM audit WHERE time < ?");
    if (stmt == NULL)
        return;
    sqlite3_bind_int64(stmt, 1, before);
    finish(self, stmt);
}

void
clawt_fed_store_nonce_add(
    ClawtFedStore *self,
    const gchar   *fleet,
    const gchar   *nonce,
    gint64         time
){
    sqlite3_stmt *stmt;

    g_return_if_fail(CLAWT_IS_FED_STORE(self));
    stmt = prepare(self, "INSERT OR REPLACE INTO nonces(fleet, nonce, time) VALUES (?, ?, ?)");
    if (stmt == NULL)
        return;
    sqlite3_bind_text(stmt, 1, fleet, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, nonce, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, time);
    finish(self, stmt);
}

JsonArray *
clawt_fed_store_nonces(
    ClawtFedStore *self,
    gint64         since
){
    JsonArray *out = json_array_new();
    sqlite3_stmt *stmt;

    g_return_val_if_fail(CLAWT_IS_FED_STORE(self), out);
    stmt = prepare(self, "DELETE FROM nonces WHERE time < ?");
    if (stmt != NULL) {
        sqlite3_bind_int64(stmt, 1, since);
        finish(self, stmt);
    }
    stmt = prepare(self, "SELECT fleet, nonce, time FROM nonces ORDER BY time");
    if (stmt == NULL)
        return out;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        JsonObject *e = json_object_new();

        json_object_set_string_member(e, "fleet", (const gchar *)sqlite3_column_text(stmt, 0));
        json_object_set_string_member(e, "nonce", (const gchar *)sqlite3_column_text(stmt, 1));
        json_object_set_int_member(e, "time", sqlite3_column_int64(stmt, 2));
        json_array_add_object_element(out, e);
    }
    sqlite3_finalize(stmt);
    return out;
}

void
clawt_fed_store_outbox_put(
    ClawtFedStore *self,
    JsonObject    *out
){
    sqlite3_stmt *stmt;

    g_return_if_fail(CLAWT_IS_FED_STORE(self));
    stmt = prepare(self, "INSERT OR REPLACE INTO msg_outbox(id, alias, kind, body, room, queued) "
                         "VALUES (?, ?, ?, ?, ?, ?)");
    if (stmt == NULL)
        return;
    sqlite3_bind_text(stmt, 1, str_or(out, "id", ""), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, str_or(out, "alias", ""), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, str_or(out, "kind", ""), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, str_or(out, "body", ""), -1, SQLITE_TRANSIENT);
    if (str_or(out, "room", NULL) != NULL)
        sqlite3_bind_text(stmt, 5, str_or(out, "room", NULL), -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(stmt, 5);
    sqlite3_bind_int64(stmt, 6, json_object_get_int_member_with_default(out, "queued", 0));
    finish(self, stmt);
}

void
clawt_fed_store_outbox_remove(
    ClawtFedStore *self,
    const gchar   *id
){
    sqlite3_stmt *stmt;

    g_return_if_fail(CLAWT_IS_FED_STORE(self));
    stmt = prepare(self, "DELETE FROM msg_outbox WHERE id = ?");
    if (stmt == NULL)
        return;
    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
    finish(self, stmt);
}

JsonArray *
clawt_fed_store_outbox(
    ClawtFedStore *self
){
    JsonArray *out = json_array_new();
    sqlite3_stmt *stmt;

    g_return_val_if_fail(CLAWT_IS_FED_STORE(self), out);
    stmt = prepare(self, "SELECT id, alias, kind, body, room, queued FROM msg_outbox "
                         "ORDER BY queued, rowid");
    if (stmt == NULL)
        return out;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        JsonObject *e = json_object_new();

        json_object_set_string_member(e, "id", (const gchar *)sqlite3_column_text(stmt, 0));
        json_object_set_string_member(e, "alias", (const gchar *)sqlite3_column_text(stmt, 1));
        json_object_set_string_member(e, "kind", (const gchar *)sqlite3_column_text(stmt, 2));
        json_object_set_string_member(e, "body", (const gchar *)sqlite3_column_text(stmt, 3));
        if (sqlite3_column_type(stmt, 4) != SQLITE_NULL)
            json_object_set_string_member(e, "room", (const gchar *)sqlite3_column_text(stmt, 4));
        json_object_set_int_member(e, "queued", sqlite3_column_int64(stmt, 5));
        json_array_add_object_element(out, e);
    }
    sqlite3_finalize(stmt);
    return out;
}

void
clawt_fed_store_delivery_set(
    ClawtFedStore *self,
    const gchar   *message,
    const gchar   *room,
    const gchar   *state,
    const gchar   *detail
){
    sqlite3_stmt *stmt;

    g_return_if_fail(CLAWT_IS_FED_STORE(self));
    if (message == NULL || room == NULL || state == NULL)
        return;
    stmt = prepare(self, "INSERT OR REPLACE INTO deliveries(message, room, state, detail, time) "
                         "VALUES (?, ?, ?, ?, ?)");
    if (stmt == NULL)
        return;
    sqlite3_bind_text(stmt, 1, message, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, room, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, state, -1, SQLITE_TRANSIENT);
    if (detail != NULL)
        sqlite3_bind_text(stmt, 4, detail, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(stmt, 4);
    sqlite3_bind_int64(stmt, 5, g_get_real_time() / G_USEC_PER_SEC);
    finish(self, stmt);
}

JsonObject *
clawt_fed_store_deliveries(
    ClawtFedStore *self,
    const gchar   *room
){
    JsonObject *out = json_object_new();
    sqlite3_stmt *stmt;

    g_return_val_if_fail(CLAWT_IS_FED_STORE(self), out);
    stmt = prepare(self, "SELECT message, state, detail FROM deliveries WHERE room = ?");
    if (stmt == NULL)
        return out;
    sqlite3_bind_text(stmt, 1, room, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        JsonObject *e = json_object_new();

        json_object_set_string_member(e, "state", (const gchar *)sqlite3_column_text(stmt, 1));
        if (sqlite3_column_type(stmt, 2) != SQLITE_NULL)
            json_object_set_string_member(e, "detail", (const gchar *)sqlite3_column_text(stmt, 2));
        json_object_set_object_member(out, (const gchar *)sqlite3_column_text(stmt, 0), e);
    }
    sqlite3_finalize(stmt);
    return out;
}

void
clawt_fed_store_kv_set(
    ClawtFedStore *self,
    const gchar   *table,
    const gchar   *key,
    JsonObject    *value
){
    sqlite3_stmt *stmt;

    g_return_if_fail(CLAWT_IS_FED_STORE(self));
    if (value == NULL) {
        stmt = prepare(self, "DELETE FROM kv WHERE tab = ? AND key = ?");
        if (stmt == NULL)
            return;
        sqlite3_bind_text(stmt, 1, table, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, key, -1, SQLITE_TRANSIENT);
        finish(self, stmt);
        return;
    }
    stmt = prepare(self, "INSERT OR REPLACE INTO kv(tab, key, value) VALUES (?, ?, ?)");
    if (stmt == NULL)
        return;
    {
        g_autofree gchar *text = text_of(value);

        sqlite3_bind_text(stmt, 1, table, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, key, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, text, -1, SQLITE_TRANSIENT);
        finish(self, stmt);
    }
}

JsonObject *
clawt_fed_store_kv_all(
    ClawtFedStore *self,
    const gchar   *table
){
    JsonObject *out = json_object_new();
    sqlite3_stmt *stmt;

    g_return_val_if_fail(CLAWT_IS_FED_STORE(self), out);
    stmt = prepare(self, "SELECT key, value FROM kv WHERE tab = ?");
    if (stmt == NULL)
        return out;
    sqlite3_bind_text(stmt, 1, table, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        JsonObject *value = object_of((const gchar *)sqlite3_column_text(stmt, 1));

        if (value != NULL)
            json_object_set_object_member(out, (const gchar *)sqlite3_column_text(stmt, 0), value);
    }
    sqlite3_finalize(stmt);
    return out;
}
