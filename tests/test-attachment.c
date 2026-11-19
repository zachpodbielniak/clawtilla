/*
 * test-attachment.c - Files an agent sends to its operator
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * An id comes off the wire and is turned into a filename, which is the
 * one thing here worth being paranoid about: everything else is a copy.
 */

#include "clawtilla.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "clawt-test-util.h"

typedef struct {
    gchar *dir;
    gchar *source;
} Fixture;

static void
fixture_setup(Fixture *fixture)
{
    g_autoptr(GError) error = NULL;

    fixture->dir = g_dir_make_tmp("clawt-attachment-XXXXXX", NULL);
    fixture->source = g_build_filename(fixture->dir, "screenshot.png", NULL);

    /*
     * With an embedded NUL, because that is what separates a copy from a
     * string: anything that treated the bytes as text would stop here.
     */
    g_file_set_contents(fixture->source, "\211PNG\0\r\n\032\n", 9, &error);
    g_assert_no_error(error);
}

static void
fixture_teardown(Fixture *fixture)
{
    /*
     * The tree, not the strings alone.  Every test here writes an
     * attachment store *inside* this directory, so freeing the path and
     * walking away left one /tmp/clawt-attachment-XXXXXX per test per
     * run -- a thousand of them had accumulated before anybody looked.
     */
    clawt_test_remove_tree(fixture->dir);

    g_free(fixture->dir);
    g_free(fixture->source);
}

static void
test_a_file_is_copied_not_referenced(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autofree gchar *id = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *contents = NULL;
    gsize length = 0;
    g_autofree gchar *store = NULL;

    fixture_setup(&fixture);
    store = g_build_filename(fixture.dir, "attachments", NULL);

    id = clawt_attachment_store(store, fixture.source, &error);
    g_assert_no_error(error);
    g_assert_nonnull(id);

    /*
     * The agent may rewrite or delete its own copy afterwards, which is
     * the whole reason the bytes are taken at send time.
     */
    g_assert_cmpint(g_unlink(fixture.source), ==, 0);

    path = clawt_attachment_path(store, id);
    g_assert_nonnull(path);
    g_assert_true(g_file_get_contents(path, &contents, &length, NULL));

    /* Nine bytes, not four: the NUL is in the middle. */
    g_assert_cmpuint(length, ==, 9);
    g_assert_cmpint(memcmp(contents, "\211PNG\0\r\n\032\n", 9), ==, 0);

    fixture_teardown(&fixture);
}

/*
 * The original name is carried in the id, so a client can label the file
 * without a second field to keep in step with it.
 */
static void
test_the_name_survives(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *id = NULL;
    g_autofree gchar *name = NULL;
    g_autofree gchar *store = NULL;

    fixture_setup(&fixture);
    store = g_build_filename(fixture.dir, "attachments", NULL);

    id = clawt_attachment_store(store, fixture.source, NULL);
    g_assert_nonnull(id);

    name = clawt_attachment_name(id);
    g_assert_cmpstr(name, ==, "screenshot.png");

    fixture_teardown(&fixture);
}

/*
 * A name is sanitised on the way in rather than escaped at each use --
 * the same rule agent ids follow.  Nothing that reaches the filesystem
 * can contain a separator or a "..".
 */
static void
test_a_hostile_name_becomes_harmless(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *hostile = NULL;
    g_autofree gchar *id = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *store = NULL;
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture);
    store = g_build_filename(fixture.dir, "attachments", NULL);

    hostile = g_build_filename(fixture.dir, "..", "escape me.png", NULL);
    g_file_set_contents(hostile, "x", 1, &error);
    g_assert_no_error(error);

    id = clawt_attachment_store(store, hostile, NULL);
    g_assert_nonnull(id);

    /* The space is gone and nothing else came with it. */
    g_assert_null(strchr(id, ' '));
    g_assert_null(strchr(id, G_DIR_SEPARATOR));
    g_assert_null(strstr(id, ".."));

    path = clawt_attachment_path(store, id);
    g_assert_nonnull(path);
    g_assert_true(g_str_has_prefix(path, store));

    g_unlink(hostile);
    fixture_teardown(&fixture);
}

/*
 * And an id that is not one of ours resolves to nothing at all.
 *
 * This is the check that matters: the id arrives in an IPC payload, and
 * without it a request for a path of somebody's choosing would read a
 * file the daemon was never asked to serve.
 */
static void
test_an_id_from_the_wire_is_checked(void)
{
    static const gchar *const hostile[] = {
        "../../secrets/token",
        "..",
        ".hidden",
        "a/b",
        "a\\b",
        "",
        NULL
    };
    gsize i;

    for (i = 0; hostile[i] != NULL; i++) {
        g_autofree gchar *path = clawt_attachment_path("/tmp/store",
                                                       hostile[i]);

        g_assert_null(path);
    }

    g_assert_null(clawt_attachment_path("/tmp/store", NULL));

    /* ...and an ordinary one still resolves. */
    {
        g_autofree gchar *ok = clawt_attachment_path("/tmp/store",
                                                     "0abc-shot.png");

        g_assert_cmpstr(ok, ==, "/tmp/store/0abc-shot.png");
    }
}

static void
test_a_missing_file_is_refused(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *id = clawt_attachment_store("/tmp/store",
                                                  "/nowhere/at/all.png",
                                                  &error);

    /*
     * Reported rather than dropped: an attachment that silently did not
     * arrive is worse than one that was refused, because the agent goes
     * on believing it sent it.
     */
    g_assert_null(id);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND);
}

/* Every ordinary basename, including adjacent dots, must remain downloadable. */
static void
test_dotted_names_round_trip(void)
{
	static const gchar *const names[] = { "report..txt", "...", ".hidden..txt", NULL };
	Fixture fixture = { 0 };
	g_autofree gchar *store = NULL;
	guint i;

	fixture_setup(&fixture);
	store = g_build_filename(fixture.dir, "attachments", NULL);
	for (i = 0; names[i] != NULL; i++) {
		g_autofree gchar *source = g_build_filename(fixture.dir, names[i], NULL);
		g_autofree gchar *id = NULL;
		g_autofree gchar *path = NULL;
		g_autofree gchar *name = NULL;
		g_autofree gchar *contents = NULL;
		g_autoptr(GError) error = NULL;

		g_assert_true(g_file_set_contents(source, "attachment bytes", -1, &error));
		g_assert_no_error(error);
		id = clawt_attachment_store(store, source, &error);
		g_assert_no_error(error);
		g_assert_nonnull(id);
		path = clawt_attachment_path(store, id);
		g_assert_nonnull(path);
		g_assert_true(g_file_get_contents(path, &contents, NULL, &error));
		g_assert_no_error(error);
		g_assert_cmpstr(contents, ==, "attachment bytes");
		name = clawt_attachment_name(id);
		g_assert_cmpstr(name, ==, names[i]);
	}
	fixture_teardown(&fixture);
}

/*
 * The block an operator's message carries, byte for byte.
 *
 * This is the text GTK's body_with_attachments() and lrg's
 * clawt_lrg_body_with_attachments() each wrote by hand before they
 * shared a builder, copied from them.  If it drifts, every message sent
 * from then on is spelled differently from every message already in a
 * transcript: the previews, the gallery and the agent's own
 * instructions all read this exact shape, and a change here breaks
 * them for one half of history with no error anywhere.
 */
static void
test_the_block_is_spelled_as_before(void)
{
    static const gchar *const expected =
        "look at these\n"
        "\n"
        "[clawtilla] Files sent with this message. Open them with your "
        "own read tool at the host path below; it runs on the host even "
        "when your shell does not.\n"
        "- shot.png\n"
        "  /home/op/.local/state/x/exchange/scout/shot.png\n"
        "  (inside your container: /exchange/scout/shot.png)\n"
        "  clawt:0abc-shot.png\n"
        "- notes.txt\n"
        "  /exchange/scout/notes.txt\n"
        "  clawt:0abd-notes.txt\n"
        "- huge.iso (could not be saved)\n";
    GString *block = clawt_attachment_block_new("look at these");
    g_autofree gchar *text = NULL;

    clawt_attachment_block_add(block, "shot.png",
                               "/home/op/.local/state/x/exchange/scout/"
                               "shot.png",
                               "/exchange/scout/shot.png", "0abc-shot.png");

    /* the same path twice: a host agent, so no container line */
    clawt_attachment_block_add(block, "notes.txt",
                               "/exchange/scout/notes.txt",
                               "/exchange/scout/notes.txt",
                               "0abd-notes.txt");
    clawt_attachment_block_add_unsaved(block, "huge.iso");

    text = g_string_free(block, FALSE);
    g_assert_cmpstr(text, ==, expected);

    /* The marker is the prefix every reader matches on. */
    g_assert_nonnull(strstr(text, CLAWT_ATTACHMENT_MARKER "."));
}

/*
 * Files with no text: the block starts the message, with no blank line
 * above it.  A leading "\n\n" would make the transcript draw an empty
 * bubble above the files.
 */
static void
test_a_block_alone_has_no_leading_gap(void)
{
    GString *block = clawt_attachment_block_new(NULL);
    g_autofree gchar *text = NULL;

    clawt_attachment_block_add(block, "a.pdf", "/x/a.pdf", NULL, NULL);
    text = g_string_free(block, FALSE);

    g_assert_true(g_str_has_prefix(text, CLAWT_ATTACHMENT_MARKER));
    g_assert_true(g_str_has_suffix(text, "- a.pdf\n  /x/a.pdf\n"));
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/attachment/copied-not-referenced",
                    test_a_file_is_copied_not_referenced);
    g_test_add_func("/attachment/name-survives", test_the_name_survives);
    g_test_add_func("/attachment/hostile-name",
                    test_a_hostile_name_becomes_harmless);
    g_test_add_func("/attachment/id-from-the-wire",
                    test_an_id_from_the_wire_is_checked);
    g_test_add_func("/attachment/missing-file",
                    test_a_missing_file_is_refused);
	g_test_add_func("/attachment/dotted-names-round-trip", test_dotted_names_round_trip);
    g_test_add_func("/attachment/block-spelled-as-before",
                    test_the_block_is_spelled_as_before);
    g_test_add_func("/attachment/block-alone", test_a_block_alone_has_no_leading_gap);

    return g_test_run();
}
