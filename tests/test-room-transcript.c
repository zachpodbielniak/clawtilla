/*
 * test-room-transcript.c - What reaches a transcript on disk
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A transcript is not a log.  It is read back into an agent's context on
 * every rebuild, so what lands in the file is handed to a model again and
 * again -- which is why redaction has to happen on the way in, and why
 * these tests are about the bytes on disk rather than about the objects
 * in memory.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>

#include "clawt-test-util.h"

/* Reads a transcript back as raw text; NULL if it was never written. */
static gchar *
read_transcript(const gchar *dir, const gchar *room_id)
{
    g_autofree gchar *name = g_strdup_printf("%s.ndjson", room_id);
    g_autofree gchar *path = NULL;
    gchar *contents = NULL;

    g_strdelimit(name, "/\\", '_');
    path = g_build_filename(dir, name, NULL);

    if (!g_file_get_contents(path, &contents, NULL, NULL))
        return NULL;

    return contents;
}

/*
 * A key in a message body does not reach the file.
 *
 * ClawtRoom::append_to_transcript() has redacted since it was written,
 * with a comment saying exactly why -- and it is inert for every room the
 * manager owns, because they are all built with a NULL transcript path.
 * The manager's own save_room() is what actually writes every transcript
 * on disk, and it did not redact, so no transcript ever had been.
 *
 * The assertion is on the *absence* of the token, which is the half that
 * matters: asserting the marker is present would pass on a build that
 * redacted one occurrence and left another.
 */
static void
test_a_secret_does_not_reach_the_transcript(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-transcript-XXXXXX", NULL);
    g_autoptr(ClawtRoomManager) rooms = clawt_room_manager_new(dir);
    g_autofree gchar *room_id = clawt_room_manager_direct_id("chief", "user");
    g_autofree gchar *written = NULL;
    ClawtRoom *room;
    g_autoptr(ClawtMessage) message = NULL;

    room = clawt_room_manager_get_direct(rooms, "chief", "user");
    g_assert_nonnull(room);

    message = clawt_message_new(room_id, "user",
                                "here is the token ghp_AbCdEfGhIjKlMnOpQrStUvWxYz012345 "
                                "and api_key=sk-0123456789abcdefghijklmnop");
    clawt_room_append(room, message, NULL);

    written = read_transcript(dir, room_id);
    g_assert_nonnull(written);

    g_assert_null(strstr(written, "ghp_AbCdEfGhIjKlMnOpQrStUvWxYz012345"));
    g_assert_null(strstr(written, "sk-0123456789abcdefghijklmnop"));
    g_assert_nonnull(strstr(written, "[REDACTED]"));

    /* The message itself is untouched: redaction is about the file. */
    g_assert_nonnull(strstr(clawt_message_get_body(message), "ghp_"));

    clawt_test_remove_tree(dir);
}

/*
 * The ordinary body survives intact.
 *
 * A redactor that ate real text would be worse than none, and this is the
 * case nobody would notice until an agent read back a mangled sentence.
 */
static void
test_an_ordinary_message_is_written_whole(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-transcript-XXXXXX", NULL);
    g_autoptr(ClawtRoomManager) rooms = clawt_room_manager_new(dir);
    g_autofree gchar *room_id = clawt_room_manager_direct_id("chief", "user");
    g_autofree gchar *written = NULL;
    ClawtRoom *room;
    g_autoptr(ClawtMessage) message = NULL;

    room = clawt_room_manager_get_direct(rooms, "chief", "user");
    message = clawt_message_new(room_id, "user",
                                "summarise yesterday's commits, please");
    clawt_room_append(room, message, NULL);

    written = read_transcript(dir, room_id);
    g_assert_nonnull(written);
    g_assert_nonnull(strstr(written, "summarise yesterday's commits, please"));
    g_assert_null(strstr(written, "[REDACTED]"));

    clawt_test_remove_tree(dir);
}

/*
 * A room created with a display name does not write a transcript named
 * after it.
 *
 * clawt_room_new()'s second argument is a transcript *path*, and the
 * manager passed the room's display name into it -- so a room somebody
 * called "Morning Standup" handed ClawtRoom a second writer aimed at a
 * made-up path.  Measured with the bug put back: a file called
 * "Morning Standup" appears in the daemon's *working directory*, holding
 * a line in the append path's own schema -- a `room` field, and
 * `timestamp` where save_room() writes `ts`.  Two writers, two shapes,
 * one room.
 *
 * So the assertion that discriminates is on the working directory, not
 * on the transcript directory: the stray file never lands in the latter,
 * and a test that only counted files there passed with the bug present.
 */
static void
test_a_named_room_writes_only_under_its_id(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-transcript-XXXXXX", NULL);
    g_autoptr(ClawtRoomManager) rooms = clawt_room_manager_new(dir);
    g_autofree gchar *written = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtMessage) message = NULL;
    g_autoptr(GDir) listing = NULL;
    const gchar *entry;
    guint files = 0;
    ClawtRoom *room;

    room = clawt_room_manager_create(rooms, "standup", "Morning Standup",
                                     &error);
    g_assert_no_error(error);
    g_assert_nonnull(room);

    message = clawt_message_new("standup", "user", "morning");
    clawt_room_append(room, message, NULL);

    written = read_transcript(dir, "standup");
    g_assert_nonnull(written);
    g_assert_nonnull(strstr(written, "morning"));

    /*
     * Nothing named for the display name, wherever the process happens
     * to be running.  This is the assertion the bug fails.
     */
    g_assert_false(g_file_test("Morning Standup", G_FILE_TEST_EXISTS));

    /* And exactly one transcript, named for the id. */
    listing = g_dir_open(dir, 0, NULL);
    g_assert_nonnull(listing);

    while ((entry = g_dir_read_name(listing)) != NULL) {
        files++;
        g_assert_cmpstr(entry, ==, "standup.ndjson");
    }

    g_assert_cmpuint(files, ==, 1);

    clawt_test_remove_tree(dir);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/transcript/secret-does-not-reach-the-file",
                    test_a_secret_does_not_reach_the_transcript);
    g_test_add_func("/transcript/ordinary-message-is-whole",
                    test_an_ordinary_message_is_written_whole);
    g_test_add_func("/transcript/named-room-writes-only-under-its-id",
                    test_a_named_room_writes_only_under_its_id);

    return g_test_run();
}
