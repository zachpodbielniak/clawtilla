/*
 * clawt-revisions.c - Every version of what shapes the fleet
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include <sqlite3.h>
#include <string.h>

#include "clawtilla.h"
#include "config/clawt-revisions.h"

static const gchar SCHEMA_SQL[] =
    "CREATE TABLE IF NOT EXISTS revisions ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  kind TEXT NOT NULL,"
    "  subject TEXT NOT NULL,"
    "  at INTEGER NOT NULL,"
    "  author TEXT,"
    "  note TEXT,"
    "  text TEXT NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS revisions_subject "
    "  ON revisions(kind, subject, id);"
    "CREATE TABLE IF NOT EXISTS skill_test_runs ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  skill TEXT NOT NULL,"
    "  test TEXT NOT NULL,"
    "  revision INTEGER NOT NULL DEFAULT 0,"
    "  agent TEXT,"
    "  task_id TEXT,"
    "  at INTEGER NOT NULL,"
    "  state TEXT NOT NULL,"
    "  detail TEXT"
    ");"
    "CREATE INDEX IF NOT EXISTS skill_test_runs_skill "
    "  ON skill_test_runs(skill, test, id);"
    "CREATE INDEX IF NOT EXISTS skill_test_runs_task "
    "  ON skill_test_runs(task_id);";

struct _ClawtRevisions {
    GObject  parent_instance;
    sqlite3 *db;
};

G_DEFINE_FINAL_TYPE(ClawtRevisions, clawt_revisions, G_TYPE_OBJECT)

static void
clawt_revisions_finalize(GObject *object)
{
    ClawtRevisions *self = CLAWT_REVISIONS(object);

    g_clear_pointer(&self->db, sqlite3_close);

    G_OBJECT_CLASS(clawt_revisions_parent_class)->finalize(object);
}

static void
clawt_revisions_class_init(ClawtRevisionsClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clawt_revisions_finalize;
}

static void
clawt_revisions_init(ClawtRevisions *self)
{
    (void)self;
}

ClawtRevisions *
clawt_revisions_new(const gchar *path, GError **error)
{
    g_autoptr(ClawtRevisions) self = NULL;
    g_autofree gchar *dir = NULL;

    g_return_val_if_fail(path != NULL, NULL);

    dir = g_path_get_dirname(path);
    if (!clawt_ensure_dir(dir, 0700, error))
        return NULL;

    self = g_object_new(CLAWT_TYPE_REVISIONS, NULL);

    if (sqlite3_open(path, &self->db) != SQLITE_OK) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "cannot open %s: %s", path, sqlite3_errmsg(self->db));
        return NULL;
    }

    sqlite3_busy_timeout(self->db, 2000);
    sqlite3_exec(self->db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);

    if (sqlite3_exec(self->db, SCHEMA_SQL, NULL, NULL, NULL) != SQLITE_OK) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "cannot create the revisions table: %s",
                    sqlite3_errmsg(self->db));
        return NULL;
    }

    return (ClawtRevisions *)g_steal_pointer(&self);
}

/* The newest revision of a subject: its id and text, or 0. */
static gint64
newest(ClawtRevisions  *self,
       const gchar     *kind,
       const gchar     *subject,
       gchar          **out_text)
{
    sqlite3_stmt *stmt = NULL;
    gint64 id = 0;

    if (sqlite3_prepare_v2(self->db,
            "SELECT id, text FROM revisions WHERE kind = ? AND subject = ? "
            "ORDER BY id DESC LIMIT 1", -1, &stmt, NULL) != SQLITE_OK)
        return 0;

    sqlite3_bind_text(stmt, 1, kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, subject, -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        id = sqlite3_column_int64(stmt, 0);
        if (out_text != NULL)
            *out_text = g_strdup((const gchar *)sqlite3_column_text(stmt, 1));
    }

    sqlite3_finalize(stmt);

    return id;
}

gint64
clawt_revisions_record(ClawtRevisions  *self,
                       const gchar     *kind,
                       const gchar     *subject,
                       const gchar     *text,
                       const gchar     *author,
                       const gchar     *note,
                       GError         **error)
{
    g_autofree gchar *kept = NULL;
    g_autofree gchar *last = NULL;
    sqlite3_stmt *stmt = NULL;
    gint64 last_id;
    gint64 id;

    g_return_val_if_fail(CLAWT_IS_REVISIONS(self), 0);
    g_return_val_if_fail(kind != NULL && subject != NULL, 0);

    if (text == NULL)
        text = "";

    if (strlen(text) > CLAWT_REVISIONS_MAX_BYTES) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "%s %s is larger than a revision keeps (%d bytes)",
                    kind, subject, CLAWT_REVISIONS_MAX_BYTES);
        return 0;
    }

    /* A config's literal secret, if one slipped in, is not copied here. */
    kept = (g_strcmp0(kind, "config") == 0) ? clawt_redact_secrets(text)
                                            : g_strdup(text);

    last_id = newest(self, kind, subject, &last);

    if (last_id > 0 && g_strcmp0(last, kept) == 0)
        return last_id;

    if (sqlite3_prepare_v2(self->db,
            "INSERT INTO revisions (kind, subject, at, author, note, text) "
            "VALUES (?, ?, ?, ?, ?, ?)", -1, &stmt, NULL) != SQLITE_OK) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "cannot keep a revision: %s", sqlite3_errmsg(self->db));
        return 0;
    }

    sqlite3_bind_text(stmt, 1, kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, subject, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, g_get_real_time() / G_USEC_PER_SEC);
    sqlite3_bind_text(stmt, 4, author, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, note, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, kept, -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "cannot keep a revision: %s", sqlite3_errmsg(self->db));
        sqlite3_finalize(stmt);
        return 0;
    }

    sqlite3_finalize(stmt);
    id = sqlite3_last_insert_rowid(self->db);

    return id;
}

JsonNode *
clawt_revisions_list(ClawtRevisions *self,
                     const gchar    *kind,
                     const gchar    *subject,
                     guint           limit)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(GString) sql = g_string_new(
        "SELECT id, kind, subject, at, author, note, length(text) "
        "FROM revisions WHERE 1");
    sqlite3_stmt *stmt = NULL;
    gint bind = 1;

    g_return_val_if_fail(CLAWT_IS_REVISIONS(self), NULL);

    if (kind != NULL)
        g_string_append(sql, " AND kind = ?");
    if (subject != NULL)
        g_string_append(sql, " AND subject = ?");
    g_string_append(sql, " ORDER BY id DESC LIMIT ?");

    json_builder_begin_array(builder);

    if (sqlite3_prepare_v2(self->db, sql->str, -1, &stmt, NULL) == SQLITE_OK) {
        if (kind != NULL)
            sqlite3_bind_text(stmt, bind++, kind, -1, SQLITE_TRANSIENT);
        if (subject != NULL)
            sqlite3_bind_text(stmt, bind++, subject, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, bind, limit > 0 ? (gint)limit : 100);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "id");
            json_builder_add_int_value(builder, sqlite3_column_int64(stmt, 0));
            json_builder_set_member_name(builder, "kind");
            json_builder_add_string_value(builder,
                (const gchar *)sqlite3_column_text(stmt, 1));
            json_builder_set_member_name(builder, "subject");
            json_builder_add_string_value(builder,
                (const gchar *)sqlite3_column_text(stmt, 2));
            json_builder_set_member_name(builder, "at");
            json_builder_add_int_value(builder, sqlite3_column_int64(stmt, 3));
            if (sqlite3_column_type(stmt, 4) != SQLITE_NULL) {
                json_builder_set_member_name(builder, "author");
                json_builder_add_string_value(builder,
                    (const gchar *)sqlite3_column_text(stmt, 4));
            }
            if (sqlite3_column_type(stmt, 5) != SQLITE_NULL) {
                json_builder_set_member_name(builder, "note");
                json_builder_add_string_value(builder,
                    (const gchar *)sqlite3_column_text(stmt, 5));
            }
            json_builder_set_member_name(builder, "bytes");
            json_builder_add_int_value(builder, sqlite3_column_int64(stmt, 6));
            json_builder_end_object(builder);
        }

        sqlite3_finalize(stmt);
    }

    json_builder_end_array(builder);

    return json_builder_get_root(builder);
}

gchar *
clawt_revisions_get(ClawtRevisions  *self,
                    gint64           id,
                    gchar          **out_kind,
                    gchar          **out_subject)
{
    sqlite3_stmt *stmt = NULL;
    gchar *text = NULL;

    g_return_val_if_fail(CLAWT_IS_REVISIONS(self), NULL);

    if (sqlite3_prepare_v2(self->db,
            "SELECT kind, subject, text FROM revisions WHERE id = ?", -1,
            &stmt, NULL) != SQLITE_OK)
        return NULL;

    sqlite3_bind_int64(stmt, 1, id);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        if (out_kind != NULL)
            *out_kind = g_strdup((const gchar *)sqlite3_column_text(stmt, 0));
        if (out_subject != NULL)
            *out_subject = g_strdup(
                (const gchar *)sqlite3_column_text(stmt, 1));
        text = g_strdup((const gchar *)sqlite3_column_text(stmt, 2));
    }

    sqlite3_finalize(stmt);

    return text;
}

guint
clawt_revisions_prune(ClawtRevisions *self, guint keep)
{
    sqlite3_stmt *stmt = NULL;
    guint removed = 0;

    g_return_val_if_fail(CLAWT_IS_REVISIONS(self), 0);

    /* Everything but the newest @keep of each subject. */
    if (sqlite3_prepare_v2(self->db,
            "DELETE FROM revisions WHERE id IN ("
            "  SELECT id FROM (SELECT id, ROW_NUMBER() OVER "
            "    (PARTITION BY kind, subject ORDER BY id DESC) AS n "
            "    FROM revisions) WHERE n > ?)", -1, &stmt, NULL) != SQLITE_OK)
        return 0;

    sqlite3_bind_int(stmt, 1, (gint)MAX(keep, 1));

    if (sqlite3_step(stmt) == SQLITE_DONE)
        removed = (guint)sqlite3_changes(self->db);

    sqlite3_finalize(stmt);

    return removed;
}

/* ── The diff ────────────────────────────────────────────────────── */

/* Line by line: past this many cells of the comparison table, it says so. */
#define DIFF_MAX_CELLS (8 * 1024 * 1024)
#define DIFF_CONTEXT (3)

typedef struct {
    gchar op;      /* ' ', '-', '+' */
    guint a_line;  /* 0-based in before, for ' ' and '-' */
    guint b_line;  /* 0-based in after, for ' ' and '+' */
} DiffOp;

/* Lines without their newlines; a final newline makes no empty line. */
static gchar **
split_lines(const gchar *text, guint *out_n)
{
    gchar **lines = g_strsplit(text != NULL ? text : "", "\n", -1);
    guint n = g_strv_length(lines);

    if (n > 0 && *lines[n - 1] == '\0') {
        g_free(lines[n - 1]);
        lines[n - 1] = NULL;
        n--;
    }

    *out_n = n;
    return lines;
}

gchar *
clawt_revisions_diff(const gchar *before,
                     const gchar *after,
                     const gchar *label_before,
                     const gchar *label_after)
{
    g_auto(GStrv) a = NULL;
    g_auto(GStrv) b = NULL;
    g_autoptr(GArray) ops = NULL;
    g_autoptr(GString) out = NULL;
    g_autofree guint32 *table = NULL;
    guint n;
    guint m;
    guint prefix = 0;
    guint suffix = 0;
    guint rows;
    guint cols;
    guint i;
    guint j;
    guint k;

    if (g_strcmp0(before, after) == 0)
        return g_strdup("");

    a = split_lines(before, &n);
    b = split_lines(after, &m);
    out = g_string_new(NULL);
    g_string_append_printf(out, "--- %s\n+++ %s\n",
                           label_before != NULL ? label_before : "before",
                           label_after != NULL ? label_after : "after");

    /* The common head and tail need no table. */
    while (prefix < n && prefix < m && g_strcmp0(a[prefix], b[prefix]) == 0)
        prefix++;
    while (suffix < n - prefix && suffix < m - prefix &&
           g_strcmp0(a[n - 1 - suffix], b[m - 1 - suffix]) == 0)
        suffix++;

    rows = n - prefix - suffix;
    cols = m - prefix - suffix;

    if ((guint64)(rows + 1) * (guint64)(cols + 1) > DIFF_MAX_CELLS) {
        g_string_append_printf(out,
            "@@ too large to compare line by line: %u lines against %u "
            "@@\n", n, m);
        return g_string_free(g_steal_pointer(&out), FALSE);
    }

    /* table[i][j]: the longest common run of a[i..] and b[j..], middle only */
    table = g_new0(guint32, (gsize)(rows + 1) * (cols + 1));

    for (i = rows; i-- > 0;) {
        for (j = cols; j-- > 0;) {
            guint32 *cell = &table[(gsize)i * (cols + 1) + j];

            if (g_strcmp0(a[prefix + i], b[prefix + j]) == 0)
                *cell = table[(gsize)(i + 1) * (cols + 1) + j + 1] + 1;
            else
                *cell = MAX(table[(gsize)(i + 1) * (cols + 1) + j],
                            table[(gsize)i * (cols + 1) + j + 1]);
        }
    }

    /* The whole script: head, the middle walked through the table, tail. */
    ops = g_array_new(FALSE, FALSE, sizeof(DiffOp));

    for (k = 0; k < prefix; k++) {
        DiffOp op = { ' ', k, k };

        g_array_append_val(ops, op);
    }

    i = 0;
    j = 0;
    while (i < rows || j < cols) {
        DiffOp op;

        if (i < rows && j < cols &&
            g_strcmp0(a[prefix + i], b[prefix + j]) == 0) {
            op.op = ' ';
            op.a_line = prefix + i++;
            op.b_line = prefix + j++;
        } else if (i < rows &&
                   (j == cols ||
                    table[(gsize)(i + 1) * (cols + 1) + j] >=
                        table[(gsize)i * (cols + 1) + j + 1])) {
            /* a removal before its replacement, as diff(1) writes it */
            op.op = '-';
            op.a_line = prefix + i++;
            op.b_line = prefix + j;
        } else {
            op.op = '+';
            op.a_line = prefix + i;
            op.b_line = prefix + j++;
        }

        g_array_append_val(ops, op);
    }

    for (k = 0; k < suffix; k++) {
        DiffOp op = { ' ', n - suffix + k, m - suffix + k };

        g_array_append_val(ops, op);
    }

    /*
     * Hunks: each change with three lines of context either side, joined
     * when their context would touch.
     */
    k = 0;
    while (k < ops->len) {
        guint start;
        guint end;
        guint a_start;
        guint b_start;
        guint a_count = 0;
        guint b_count = 0;
        guint x;

        while (k < ops->len && g_array_index(ops, DiffOp, k).op == ' ')
            k++;
        if (k == ops->len)
            break;

        start = (k > DIFF_CONTEXT) ? k - DIFF_CONTEXT : 0;
        end = k;

        /* extend while another change is within twice the context */
        for (x = k; x < ops->len; x++) {
            if (g_array_index(ops, DiffOp, x).op != ' ')
                end = x;
            else if (x - end > 2 * DIFF_CONTEXT)
                break;
        }

        end = MIN(end + DIFF_CONTEXT, ops->len - 1);

        for (x = start; x <= end; x++) {
            gchar op = g_array_index(ops, DiffOp, x).op;

            if (op != '+')
                a_count++;
            if (op != '-')
                b_count++;
        }

        a_start = g_array_index(ops, DiffOp, start).a_line;
        b_start = g_array_index(ops, DiffOp, start).b_line;

        g_string_append_printf(out, "@@ -%u,%u +%u,%u @@\n",
                               a_count > 0 ? a_start + 1 : a_start, a_count,
                               b_count > 0 ? b_start + 1 : b_start, b_count);

        for (x = start; x <= end; x++) {
            DiffOp *op = &g_array_index(ops, DiffOp, x);

            g_string_append_printf(out, "%c%s\n", op->op,
                                   op->op == '+' ? b[op->b_line]
                                                 : a[op->a_line]);
        }

        k = end + 1;
    }

    return g_string_free(g_steal_pointer(&out), FALSE);
}

/* ── Skill test runs ─────────────────────────────────────────────── */

gint64
clawt_revisions_newest_id(ClawtRevisions *self,
                          const gchar    *kind,
                          const gchar    *subject)
{
    g_return_val_if_fail(CLAWT_IS_REVISIONS(self), 0);

    return newest(self, kind, subject, NULL);
}

gint64
clawt_revisions_start_test_run(ClawtRevisions *self,
                               const gchar    *skill,
                               const gchar    *test,
                               gint64          revision,
                               const gchar    *agent,
                               const gchar    *task_id)
{
    sqlite3_stmt *stmt = NULL;
    gint64 id = 0;

    g_return_val_if_fail(CLAWT_IS_REVISIONS(self), 0);

    if (sqlite3_prepare_v2(self->db,
            "INSERT INTO skill_test_runs (skill, test, revision, agent, "
            "  task_id, at, state) VALUES (?, ?, ?, ?, ?, ?, 'running')",
            -1, &stmt, NULL) != SQLITE_OK)
        return 0;

    sqlite3_bind_text(stmt, 1, skill, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, test, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, revision);
    sqlite3_bind_text(stmt, 4, agent, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, task_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 6, g_get_real_time() / G_USEC_PER_SEC);

    if (sqlite3_step(stmt) == SQLITE_DONE)
        id = sqlite3_last_insert_rowid(self->db);

    sqlite3_finalize(stmt);

    return id;
}

gboolean
clawt_revisions_test_run_for_task(ClawtRevisions  *self,
                                  const gchar     *task_id,
                                  gchar          **out_skill,
                                  gchar          **out_test)
{
    sqlite3_stmt *stmt = NULL;
    gboolean found = FALSE;

    g_return_val_if_fail(CLAWT_IS_REVISIONS(self), FALSE);

    if (task_id == NULL ||
        sqlite3_prepare_v2(self->db,
            "SELECT skill, test FROM skill_test_runs WHERE task_id = ? AND "
            "state = 'running'", -1, &stmt, NULL) != SQLITE_OK)
        return FALSE;

    sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        found = TRUE;
        if (out_skill != NULL)
            *out_skill = g_strdup((const gchar *)sqlite3_column_text(stmt, 0));
        if (out_test != NULL)
            *out_test = g_strdup((const gchar *)sqlite3_column_text(stmt, 1));
    }

    sqlite3_finalize(stmt);

    return found;
}

void
clawt_revisions_finish_test_run(ClawtRevisions *self,
                                const gchar    *task_id,
                                const gchar    *state,
                                const gchar    *detail)
{
    sqlite3_stmt *stmt = NULL;

    g_return_if_fail(CLAWT_IS_REVISIONS(self));

    if (sqlite3_prepare_v2(self->db,
            "UPDATE skill_test_runs SET state = ?, detail = ? "
            "WHERE task_id = ? AND state = 'running'", -1, &stmt,
            NULL) != SQLITE_OK)
        return;

    sqlite3_bind_text(stmt, 1, state, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, detail, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, task_id, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

JsonNode *
clawt_revisions_last_test_run(ClawtRevisions *self,
                              const gchar    *skill,
                              const gchar    *test)
{
    sqlite3_stmt *stmt = NULL;
    JsonNode *node = NULL;

    g_return_val_if_fail(CLAWT_IS_REVISIONS(self), NULL);

    if (sqlite3_prepare_v2(self->db,
            "SELECT at, state, detail, revision, agent, task_id "
            "FROM skill_test_runs WHERE skill = ? AND test = ? "
            "ORDER BY id DESC LIMIT 1", -1, &stmt, NULL) != SQLITE_OK)
        return NULL;

    sqlite3_bind_text(stmt, 1, skill, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, test, -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        g_autoptr(JsonBuilder) builder = json_builder_new();
        static const gchar *const text_columns[] = {
            NULL, "state", "detail", NULL, "agent", "task"
        };
        guint c;

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "at");
        json_builder_add_int_value(builder, sqlite3_column_int64(stmt, 0));
        json_builder_set_member_name(builder, "revision");
        json_builder_add_int_value(builder, sqlite3_column_int64(stmt, 3));

        for (c = 0; c < G_N_ELEMENTS(text_columns); c++) {
            if (text_columns[c] == NULL ||
                sqlite3_column_type(stmt, (gint)c) == SQLITE_NULL)
                continue;
            json_builder_set_member_name(builder, text_columns[c]);
            json_builder_add_string_value(builder,
                (const gchar *)sqlite3_column_text(stmt, (gint)c));
        }

        json_builder_end_object(builder);
        node = json_builder_get_root(builder);
    }

    sqlite3_finalize(stmt);

    return node;
}
