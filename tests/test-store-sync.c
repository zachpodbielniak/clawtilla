/*
 * test-store-sync.c - A write the main loop makes does not wait on the disk
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The daemon's stores are written on its main loop, the same loop that
 * accepts on the control socket.  A store left at SQLite's defaults --
 * a rollback journal and synchronous=FULL -- calls fsync() several times
 * per write, and on a disk that is already saturated one fsync can take
 * tens of seconds.  The loop then accepts nothing, the listen backlog
 * fills, and every client is refused with EAGAIN while the daemon is
 * alive and "working".
 *
 * Most stores already open themselves WAL with synchronous=NORMAL,
 * where a commit appends to the log and only a checkpoint syncs.  This
 * asserts that the ones that did not now do, by counting the syncs
 * SQLite asks the operating system for: a VFS that wraps the default
 * one and counts every xSync (and every delete that syncs its
 * directory) is registered as the default before any store opens.
 *
 * The count is taken after one write, so the sync that writes a fresh
 * WAL's header is not charged to the writes being measured -- that one
 * happens once per log, not once per write.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>
#include <sqlite3.h>
#include <string.h>

#include "clawt-test-util.h"

#include "chat/clawt-read-store.h"
#include "config/clawt-revisions.h"
#include "core/clawt-notice-store.h"
#include "decision/clawt-decision-store.h"
#include "task/clawt-handoff-store.h"

#define WRITES 20

/* ── The counting VFS ────────────────────────────────────────────── */

typedef struct {
    sqlite3_file  base;
    sqlite3_file *real;
} CountFile;

static sqlite3_vfs *real_vfs;
static sqlite3_vfs  count_vfs;
static guint        sync_count;

static int
count_close(sqlite3_file *file)
{
    CountFile *self = (CountFile *)file;
    int rc = self->real->pMethods->xClose(self->real);

    self->base.pMethods = NULL;
    return rc;
}

static int
count_read(sqlite3_file *file, void *buffer, int amount, sqlite3_int64 offset)
{
    CountFile *self = (CountFile *)file;

    return self->real->pMethods->xRead(self->real, buffer, amount, offset);
}

static int
count_write(sqlite3_file *file, const void *buffer, int amount,
            sqlite3_int64 offset)
{
    CountFile *self = (CountFile *)file;

    return self->real->pMethods->xWrite(self->real, buffer, amount, offset);
}

static int
count_truncate(sqlite3_file *file, sqlite3_int64 size)
{
    CountFile *self = (CountFile *)file;

    return self->real->pMethods->xTruncate(self->real, size);
}

static int
count_sync(sqlite3_file *file, int flags)
{
    CountFile *self = (CountFile *)file;

    sync_count++;
    return self->real->pMethods->xSync(self->real, flags);
}

static int
count_file_size(sqlite3_file *file, sqlite3_int64 *size)
{
    CountFile *self = (CountFile *)file;

    return self->real->pMethods->xFileSize(self->real, size);
}

static int
count_lock(sqlite3_file *file, int lock)
{
    CountFile *self = (CountFile *)file;

    return self->real->pMethods->xLock(self->real, lock);
}

static int
count_unlock(sqlite3_file *file, int lock)
{
    CountFile *self = (CountFile *)file;

    return self->real->pMethods->xUnlock(self->real, lock);
}

static int
count_check_reserved_lock(sqlite3_file *file, int *out)
{
    CountFile *self = (CountFile *)file;

    return self->real->pMethods->xCheckReservedLock(self->real, out);
}

static int
count_file_control(sqlite3_file *file, int op, void *arg)
{
    CountFile *self = (CountFile *)file;

    return self->real->pMethods->xFileControl(self->real, op, arg);
}

static int
count_sector_size(sqlite3_file *file)
{
    CountFile *self = (CountFile *)file;

    return self->real->pMethods->xSectorSize(self->real);
}

static int
count_device_characteristics(sqlite3_file *file)
{
    CountFile *self = (CountFile *)file;

    return self->real->pMethods->xDeviceCharacteristics(self->real);
}

static int
count_shm_map(sqlite3_file *file, int region, int size, int extend,
              void volatile **out)
{
    CountFile *self = (CountFile *)file;

    return self->real->pMethods->xShmMap(self->real, region, size, extend,
                                         out);
}

static int
count_shm_lock(sqlite3_file *file, int offset, int n, int flags)
{
    CountFile *self = (CountFile *)file;

    return self->real->pMethods->xShmLock(self->real, offset, n, flags);
}

static void
count_shm_barrier(sqlite3_file *file)
{
    CountFile *self = (CountFile *)file;

    self->real->pMethods->xShmBarrier(self->real);
}

static int
count_shm_unmap(sqlite3_file *file, int delete_flag)
{
    CountFile *self = (CountFile *)file;

    return self->real->pMethods->xShmUnmap(self->real, delete_flag);
}

static int
count_fetch(sqlite3_file *file, sqlite3_int64 offset, int amount, void **out)
{
    CountFile *self = (CountFile *)file;

    return self->real->pMethods->xFetch(self->real, offset, amount, out);
}

static int
count_unfetch(sqlite3_file *file, sqlite3_int64 offset, void *page)
{
    CountFile *self = (CountFile *)file;

    return self->real->pMethods->xUnfetch(self->real, offset, page);
}

static const sqlite3_io_methods count_io_methods = {
    3,
    count_close,
    count_read,
    count_write,
    count_truncate,
    count_sync,
    count_file_size,
    count_lock,
    count_unlock,
    count_check_reserved_lock,
    count_file_control,
    count_sector_size,
    count_device_characteristics,
    count_shm_map,
    count_shm_lock,
    count_shm_barrier,
    count_shm_unmap,
    count_fetch,
    count_unfetch
};

static int
count_open(sqlite3_vfs *vfs, const char *name, sqlite3_file *file, int flags,
           int *out_flags)
{
    CountFile *self = (CountFile *)file;
    int rc;

    (void)vfs;

    self->real = (sqlite3_file *)&self[1];
    rc = real_vfs->xOpen(real_vfs, name, self->real, flags, out_flags);

    /*
     * The unix VFS's methods are version 3, so every slot above has a
     * real one to forward to; a failed open leaves pMethods NULL, and
     * SQLite then closes nothing.
     */
    self->base.pMethods = (rc == SQLITE_OK && self->real->pMethods != NULL)
        ? &count_io_methods : NULL;
    return rc;
}

static int
count_delete(sqlite3_vfs *vfs, const char *name, int sync_dir)
{
    (void)vfs;

    /* Deleting a rollback journal with sync_dir set fsyncs the directory. */
    if (sync_dir)
        sync_count++;
    return real_vfs->xDelete(real_vfs, name, sync_dir);
}

static int
count_access(sqlite3_vfs *vfs, const char *name, int flags, int *out)
{
    (void)vfs;
    return real_vfs->xAccess(real_vfs, name, flags, out);
}

static int
count_full_pathname(sqlite3_vfs *vfs, const char *name, int size, char *out)
{
    (void)vfs;
    return real_vfs->xFullPathname(real_vfs, name, size, out);
}

static void *
count_dl_open(sqlite3_vfs *vfs, const char *name)
{
    (void)vfs;
    return real_vfs->xDlOpen(real_vfs, name);
}

static void
count_dl_error(sqlite3_vfs *vfs, int size, char *out)
{
    (void)vfs;
    real_vfs->xDlError(real_vfs, size, out);
}

static void (*
count_dl_sym(sqlite3_vfs *vfs, void *handle, const char *symbol))(void)
{
    (void)vfs;
    return real_vfs->xDlSym(real_vfs, handle, symbol);
}

static void
count_dl_close(sqlite3_vfs *vfs, void *handle)
{
    (void)vfs;
    real_vfs->xDlClose(real_vfs, handle);
}

static int
count_randomness(sqlite3_vfs *vfs, int size, char *out)
{
    (void)vfs;
    return real_vfs->xRandomness(real_vfs, size, out);
}

static int
count_sleep(sqlite3_vfs *vfs, int microseconds)
{
    (void)vfs;
    return real_vfs->xSleep(real_vfs, microseconds);
}

static int
count_current_time(sqlite3_vfs *vfs, double *out)
{
    (void)vfs;
    return real_vfs->xCurrentTime(real_vfs, out);
}

static int
count_get_last_error(sqlite3_vfs *vfs, int size, char *out)
{
    (void)vfs;
    return real_vfs->xGetLastError != NULL
        ? real_vfs->xGetLastError(real_vfs, size, out) : 0;
}

static int
count_current_time_int64(sqlite3_vfs *vfs, sqlite3_int64 *out)
{
    (void)vfs;
    return real_vfs->xCurrentTimeInt64(real_vfs, out);
}

static void
install_counting_vfs(void)
{
    g_assert_cmpint(sqlite3_initialize(), ==, SQLITE_OK);

    real_vfs = sqlite3_vfs_find(NULL);
    g_assert_nonnull(real_vfs);
    g_assert_cmpint(real_vfs->iVersion, >=, 2);

    memset(&count_vfs, 0, sizeof(count_vfs));
    count_vfs.iVersion = 2;
    count_vfs.szOsFile = (int)sizeof(CountFile) + real_vfs->szOsFile;
    count_vfs.mxPathname = real_vfs->mxPathname;
    count_vfs.zName = "clawt-count-syncs";
    count_vfs.xOpen = count_open;
    count_vfs.xDelete = count_delete;
    count_vfs.xAccess = count_access;
    count_vfs.xFullPathname = count_full_pathname;
    count_vfs.xDlOpen = count_dl_open;
    count_vfs.xDlError = count_dl_error;
    count_vfs.xDlSym = count_dl_sym;
    count_vfs.xDlClose = count_dl_close;
    count_vfs.xRandomness = count_randomness;
    count_vfs.xSleep = count_sleep;
    count_vfs.xCurrentTime = count_current_time;
    count_vfs.xGetLastError = count_get_last_error;
    count_vfs.xCurrentTimeInt64 = count_current_time_int64;

    /* The default, so a store's plain sqlite3_open() goes through it. */
    g_assert_cmpint(sqlite3_vfs_register(&count_vfs, 1), ==, SQLITE_OK);
}

/* ── One case per store ──────────────────────────────────────────── */

static gchar *
store_path(const gchar **out_dir, const gchar *name)
{
    gchar *dir = g_dir_make_tmp("clawt-sync-XXXXXX", NULL);

    g_assert_nonnull(dir);
    *out_dir = dir;
    return g_build_filename(dir, name, NULL);
}

static void
add_notice(ClawtNoticeStore *store, guint i)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *title = g_strdup_printf("notice %u", i);
    ClawtNotice notice = { 0 };

    notice.agent = (gchar *)"worker";
    notice.tier = CLAWT_ALERT_NOTICE;
    notice.event_kind = (gchar *)"agent.state";
    notice.source = (gchar *)"";
    notice.title = title;
    notice.created_at = 1000 + (gint64)i;

    g_assert_cmpint(clawt_notice_store_add(store, &notice, FALSE, &error), >, 0);
    g_assert_no_error(error);
}

static void
test_notice_store(void)
{
    const gchar *dir = NULL;
    g_autofree gchar *path = store_path(&dir, "notices.db");
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtNoticeStore) store = clawt_notice_store_new(path, &error);
    guint i;

    g_assert_no_error(error);
    add_notice(store, 0);

    sync_count = 0;
    for (i = 1; i <= WRITES; i++)
        add_notice(store, i);

    g_assert_cmpuint(sync_count, ==, 0);

    g_clear_object(&store);
    clawt_test_remove_tree(dir);
    g_free((gchar *)dir);
}

static void
test_read_store(void)
{
    const gchar *dir = NULL;
    g_autofree gchar *path = store_path(&dir, "reads.db");
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtReadStore) store = clawt_read_store_new(path, &error);
    guint i;

    g_assert_no_error(error);
    g_assert_true(clawt_read_store_set(store, "dm:a:b", "m-0", 1, &error));

    sync_count = 0;
    for (i = 1; i <= WRITES; i++) {
        g_autofree gchar *id = g_strdup_printf("m-%u", i);

        g_assert_true(clawt_read_store_set(store, "dm:a:b", id, (gint64)i + 1,
                                           &error));
        g_assert_no_error(error);
    }

    g_assert_cmpuint(sync_count, ==, 0);

    g_clear_object(&store);
    clawt_test_remove_tree(dir);
    g_free((gchar *)dir);
}

static void
test_revisions(void)
{
    const gchar *dir = NULL;
    g_autofree gchar *path = store_path(&dir, "revisions.db");
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtRevisions) store = clawt_revisions_new(path, &error);
    guint i;

    g_assert_no_error(error);
    g_assert_cmpint(clawt_revisions_record(store, "config", "fleet", "v0",
                                           "test", NULL, &error), >, 0);

    sync_count = 0;
    for (i = 1; i <= WRITES; i++) {
        g_autofree gchar *text = g_strdup_printf("v%u", i);

        g_assert_cmpint(clawt_revisions_record(store, "config", "fleet", text,
                                               "test", NULL, &error), >, 0);
        g_assert_no_error(error);
    }

    g_assert_cmpuint(sync_count, ==, 0);

    g_clear_object(&store);
    clawt_test_remove_tree(dir);
    g_free((gchar *)dir);
}

static void
test_decision_store(void)
{
    const gchar *dir = NULL;
    g_autofree gchar *path = store_path(&dir, "decisions.db");
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtDecisionStore) store = clawt_decision_store_new(path, &error);
    guint i;

    g_assert_no_error(error);

    for (i = 0; i <= WRITES; i++) {
        g_autofree gchar *question = g_strdup_printf("question %u?", i);
        g_autoptr(ClawtDecision) decision =
            clawt_decision_new(NULL, "chief", question);
        g_autofree gchar *id = NULL;

        if (i == 1)
            sync_count = 0;

        id = clawt_decision_store_post(store, decision, &error);
        g_assert_no_error(error);
        g_assert_nonnull(id);
    }

    g_assert_cmpuint(sync_count, ==, 0);

    g_clear_object(&store);
    clawt_test_remove_tree(dir);
    g_free((gchar *)dir);
}

static void
test_handoff_store(void)
{
    const gchar *dir = NULL;
    g_autofree gchar *path = store_path(&dir, "handoffs.db");
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtHandoffStore) store = clawt_handoff_store_new(path, &error);
    guint i;

    g_assert_no_error(error);

    for (i = 0; i <= WRITES; i++) {
        g_autofree gchar *task = g_strdup_printf("task-%u", i);
        g_autoptr(ClawtHandoff) handoff =
            clawt_handoff_new(task, "chief", "worker", "because it is yours");

        if (i == 1)
            sync_count = 0;

        g_assert_true(clawt_handoff_store_queue(store, handoff, &error));
        g_assert_no_error(error);
    }

    g_assert_cmpuint(sync_count, ==, 0);

    g_clear_object(&store);
    clawt_test_remove_tree(dir);
    g_free((gchar *)dir);
}

/*
 * The instrument, checked: a store opened with SQLite's defaults syncs on
 * every write.  Without this a VFS that silently failed to count would
 * pass every case above.
 */
static void
test_defaults_do_sync(void)
{
    const gchar *dir = NULL;
    g_autofree gchar *path = store_path(&dir, "plain.db");
    sqlite3 *db = NULL;
    guint i;

    g_assert_cmpint(sqlite3_open(path, &db), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_exec(db, "CREATE TABLE t (v INTEGER);", NULL, NULL,
                                 NULL), ==, SQLITE_OK);

    sync_count = 0;
    for (i = 0; i < WRITES; i++)
        g_assert_cmpint(sqlite3_exec(db, "INSERT INTO t VALUES (1);", NULL,
                                     NULL, NULL), ==, SQLITE_OK);

    g_assert_cmpuint(sync_count, >=, WRITES);

    sqlite3_close(db);
    clawt_test_remove_tree(dir);
    g_free((gchar *)dir);
}

gint
main(gint argc, gchar *argv[])
{
    g_test_init(&argc, &argv, NULL);

    install_counting_vfs();

    g_test_add_func("/store-sync/defaults-do-sync", test_defaults_do_sync);
    g_test_add_func("/store-sync/notices", test_notice_store);
    g_test_add_func("/store-sync/reads", test_read_store);
    g_test_add_func("/store-sync/revisions", test_revisions);
    g_test_add_func("/store-sync/decisions", test_decision_store);
    g_test_add_func("/store-sync/handoffs", test_handoff_store);

    return g_test_run();
}
