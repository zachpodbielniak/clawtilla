/*
 * clawt-usage.c - What the fleet has spent
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Every agent is a libreclaw process, and libreclaw has recorded one
 * `token_usage` row per AI turn -- tokens and cost -- since 0.24.0.  Each
 * agent's rows are in its own database, so the fleet's spend is the sum
 * over those files and has been sitting on disk, unread, the whole time.
 *
 * Read through libreclaw's own API rather than by opening its schema, for
 * the same reason `/reset` does: the table belongs to libreclaw, and a
 * copy of its column list here would be a second thing to keep in step
 * with a table we do not own.
 */

#include "clawtilla.h"

#include <glib/gstdio.h>

#include "usage/clawt-usage.h"

/*
 * One agent's place in its own history.
 *
 * Both halves are needed.  Rows are filtered by `recorded_at`, which has
 * one-second resolution, so two turns in the same second are
 * indistinguishable by time alone -- the id settles which of them has
 * already been charged.
 */
typedef struct {
    LcDatabase *db;
    gint64      last_recorded_at;
    gint64      last_id;
    gboolean    primed;
} AgentWatermark;

struct _ClawtUsage {
    GObject     parent_instance;
    GHashTable *watermarks;   /* agent id -> AgentWatermark* */
};

G_DEFINE_FINAL_TYPE(ClawtUsage, clawt_usage, G_TYPE_OBJECT)

/* ── ClawtUsageTotals ────────────────────────────────────────────── */

ClawtUsageTotals *
clawt_usage_totals_copy(const ClawtUsageTotals *self)
{
    ClawtUsageTotals *copy;

    g_return_val_if_fail(self != NULL, NULL);

    copy = g_new0(ClawtUsageTotals, 1);
    *copy = *self;

    return copy;
}

void
clawt_usage_totals_free(ClawtUsageTotals *self)
{
    g_free(self);
}

G_DEFINE_BOXED_TYPE(ClawtUsageTotals, clawt_usage_totals,
                    clawt_usage_totals_copy, clawt_usage_totals_free)

void
clawt_usage_totals_add(ClawtUsageTotals *self, const ClawtUsageTotals *other)
{
    g_return_if_fail(self != NULL);
    g_return_if_fail(other != NULL);

    self->turns         += other->turns;
    self->input_tokens  += other->input_tokens;
    self->output_tokens += other->output_tokens;
    self->cost_micros   += other->cost_micros;
}

gchar *
clawt_usage_format_cost(gint64 cost_micros)
{
    gdouble dollars = (gdouble)cost_micros / 1000000.0;

    /*
     * Anything that would round to $0.00 is shown to four places
     * instead.  A per-turn cost of a fifth of a cent is the ordinary
     * case here, and a table of "$0.00" says the wrong thing about it.
     */
    if (cost_micros != 0 && cost_micros > -10000 && cost_micros < 10000)
        return g_strdup_printf("$%.4f", dollars);

    return g_strdup_printf("$%.2f", dollars);
}

/* ── Where the database is ───────────────────────────────────────── */

gchar *
clawt_usage_database_path(const gchar *state_dir)
{
    g_return_val_if_fail(state_dir != NULL, NULL);

    return g_build_filename(state_dir, "sessions", "libreclaw.db", NULL);
}

/* ── Reading ─────────────────────────────────────────────────────── */

/*
 * Opens an agent's database for reading.
 *
 * Returns NULL without setting @error when the file is simply not there
 * -- an agent that has never started has no history, which the callers
 * report as zero rather than as a failure.  Opening would otherwise
 * *create* the file, leaving an empty database behind every time
 * somebody asked what a stopped agent had spent.
 */
static LcDatabase *
open_agent_database(const gchar *db_path, GError **error)
{
    LcDatabase *db;

    if (!g_file_test(db_path, G_FILE_TEST_EXISTS))
        return NULL;

    db = LC_DATABASE(lc_sqlite_database_new());

    if (!lc_database_open(db, db_path, error)) {
        g_object_unref(db);
        return NULL;
    }

    return db;
}

/*
 * Sums a row array into @out, tracking the highest row seen.
 *
 * @min_id, when positive, skips rows at or below it.  The time filter
 * cannot express "after this row" on its own, and re-reading a second's
 * worth of rows is how the same turn would be charged twice.
 */
static void
accumulate_rows(GPtrArray        *rows,
                gint64            min_id,
                ClawtUsageTotals *out,
                gint64           *out_max_id,
                gint64           *out_max_time)
{
    guint i;

    for (i = 0; rows != NULL && i < rows->len; i++) {
        LcDbTokenUsage *row = g_ptr_array_index(rows, i);

        if (min_id > 0 && row->id <= min_id)
            continue;

        out->turns         += 1;
        out->input_tokens  += row->input_tokens;
        out->output_tokens += row->output_tokens;
        out->cost_micros   += row->cost_usd_micros;

        if (out_max_id != NULL && row->id > *out_max_id)
            *out_max_id = row->id;
        if (out_max_time != NULL && row->recorded_at > *out_max_time)
            *out_max_time = row->recorded_at;
    }
}

gboolean
clawt_usage_read_totals(const gchar       *db_path,
                        gint64             since,
                        ClawtUsageTotals  *out,
                        GError           **error)
{
    g_autoptr(LcDatabase) db = NULL;
    GPtrArray *rows;

    g_return_val_if_fail(db_path != NULL, FALSE);
    g_return_val_if_fail(out != NULL, FALSE);

    memset(out, 0, sizeof(*out));

    db = open_agent_database(db_path, error);
    if (db == NULL)
        return (error == NULL || *error == NULL);

    rows = lc_database_query_token_usage(db, NULL, NULL, since, 0, error);
    if (rows == NULL) {
        lc_database_close(db);
        return FALSE;
    }

    accumulate_rows(rows, 0, out, NULL, NULL);

    g_ptr_array_unref(rows);
    lc_database_close(db);

    return TRUE;
}

/* ── Charging ────────────────────────────────────────────────────── */

static void
agent_watermark_free(gpointer data)
{
    AgentWatermark *mark = data;

    if (mark->db != NULL) {
        lc_database_close(mark->db);
        g_object_unref(mark->db);
    }

    g_free(mark);
}

ClawtUsage *
clawt_usage_new(void)
{
    return g_object_new(CLAWT_TYPE_USAGE, NULL);
}

gint64
clawt_usage_drain(ClawtUsage *self, const gchar *agent_id,
                  const gchar *db_path)
{
    AgentWatermark   *mark;
    ClawtUsageTotals  totals = { 0, 0, 0, 0 };
    GPtrArray        *rows;
    g_autoptr(GError) error = NULL;
    gint64            max_id;
    gint64            max_time;

    g_return_val_if_fail(CLAWT_IS_USAGE(self), 0);
    g_return_val_if_fail(agent_id != NULL, 0);
    g_return_val_if_fail(db_path != NULL, 0);

    mark = g_hash_table_lookup(self->watermarks, agent_id);
    if (mark == NULL) {
        mark = g_new0(AgentWatermark, 1);
        g_hash_table_insert(self->watermarks, g_strdup(agent_id), mark);
    }

    /*
     * The handle is kept open across drains.  Charging happens on the
     * daemon's main context whenever an agent answers, and reopening a
     * database -- which runs libreclaw's schema check every time -- is
     * not something to do on that path.
     *
     * A database that is not there yet is not cached as a failure: an
     * agent starting for the first time writes it moments later.
     */
    if (mark->db == NULL) {
        mark->db = open_agent_database(db_path, &error);

        if (mark->db == NULL) {
            if (error != NULL)
                g_debug("usage: cannot read %s: %s", db_path, error->message);
            return 0;
        }
    }

    max_id   = mark->last_id;
    max_time = mark->last_recorded_at;

    rows = lc_database_query_token_usage(mark->db, NULL, NULL,
                                         mark->last_recorded_at, 0, &error);
    if (rows == NULL) {
        g_debug("usage: cannot query %s: %s", db_path,
                error != NULL ? error->message : "unknown error");
        return 0;
    }

    accumulate_rows(rows, mark->last_id, &totals, &max_id, &max_time);
    g_ptr_array_unref(rows);

    mark->last_id          = max_id;
    mark->last_recorded_at = max_time;

    /*
     * The first sight of an agent sets the watermark and charges
     * nothing.  Everything before now was spent under some earlier
     * daemon, on work that has already finished -- charging a fresh task
     * for it would exhaust its budget before its first turn.
     */
    if (!mark->primed) {
        mark->primed = TRUE;
        return 0;
    }

    return totals.cost_micros;
}

void
clawt_usage_forget(ClawtUsage *self, const gchar *agent_id)
{
    g_return_if_fail(CLAWT_IS_USAGE(self));
    g_return_if_fail(agent_id != NULL);

    g_hash_table_remove(self->watermarks, agent_id);
}

/* ── GObject ─────────────────────────────────────────────────────── */

static void
clawt_usage_finalize(GObject *object)
{
    ClawtUsage *self = CLAWT_USAGE(object);

    g_clear_pointer(&self->watermarks, g_hash_table_unref);

    G_OBJECT_CLASS(clawt_usage_parent_class)->finalize(object);
}

static void
clawt_usage_class_init(ClawtUsageClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = clawt_usage_finalize;
}

static void
clawt_usage_init(ClawtUsage *self)
{
    self->watermarks = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, agent_watermark_free);
}
