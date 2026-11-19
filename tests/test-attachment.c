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

/* ── Reading the block back ─────────────────────────────────────────── */

/*
 * One ref per item, from every shape the block has had: an old item that
 * is itself a path, the host-path-then-container form, an agent's
 * `clawt:` item, and an operator's file named both ways -- where the id
 * wins.  A line at the margin after the block ends it.
 *
 * What breaks without it: the libregnum previews, the web previews and
 * attachment.list all read blocks through this one parser.  A second
 * ref per item draws every operator file twice and lists it twice in
 * the gallery; a path that survives past the block sprouts a preview of
 * whatever an agent happened to quote.
 */
static void
test_refs_read_every_shape(void)
{
    static const gchar *const body =
        "look at these\n\n"
        CLAWT_ATTACHMENT_MARKER ". Open them with your own read tool.\n"
        "- /home/z/old/screenshot.png\n"
        "- shot.png\n"
        "  /home/z/.clawtilla/exchange/shot.png\n"
        "  (inside your container: /exchange/shot.png)\n"
        "- notes.pdf\n"
        "  clawt:abc123-notes.pdf\n"
        "- both.txt\r\n"
        "  /host/both.txt\r\n"
        "  clawt:feed-both.txt\r\n"
        "- huge.iso (could not be saved)\n"
        "And then I quoted /etc/passwd at the margin.\n";
    g_autoptr(GPtrArray) refs = clawt_attachment_refs_parse(body);
    g_autoptr(GPtrArray) none = clawt_attachment_refs_parse("just /a/path");
    g_autofree gchar *prose = clawt_attachment_prose(body);
    ClawtAttachmentRef *ref;

    g_assert_cmpuint(refs->len, ==, 4);

    ref = g_ptr_array_index(refs, 0);
    g_assert_false(ref->daemon_copy);
    g_assert_cmpstr(ref->value, ==, "/home/z/old/screenshot.png");
    g_assert_cmpstr(ref->name, ==, "screenshot.png");

    ref = g_ptr_array_index(refs, 1);
    g_assert_false(ref->daemon_copy);
    g_assert_cmpstr(ref->value, ==, "/home/z/.clawtilla/exchange/shot.png");
    g_assert_cmpstr(ref->name, ==, "shot.png");

    ref = g_ptr_array_index(refs, 2);
    g_assert_true(ref->daemon_copy);
    g_assert_cmpstr(ref->value, ==, "abc123-notes.pdf");
    g_assert_cmpstr(ref->name, ==, "notes.pdf");

    /* named both ways, CRLF and all: the daemon's copy, once */
    ref = g_ptr_array_index(refs, 3);
    g_assert_true(ref->daemon_copy);
    g_assert_cmpstr(ref->value, ==, "feed-both.txt");
    g_assert_cmpstr(ref->name, ==, "both.txt");

    g_assert_cmpuint(none->len, ==, 0);

    /* the prose keeps both sides of the block and loses the block */
    g_assert_true(g_str_has_prefix(prose, "look at these"));
    g_assert_nonnull(strstr(prose, "And then I quoted /etc/passwd"));
    g_assert_null(strstr(prose, CLAWT_ATTACHMENT_MARKER));
    g_assert_null(strstr(prose, "clawt:"));
}

/*
 * What the shared builder writes, the shared parser reads back -- the
 * round trip every client's message makes.
 */
static void
test_refs_read_what_the_builder_wrote(void)
{
    GString *block = clawt_attachment_block_new("see");
    g_autofree gchar *body = NULL;
    g_autoptr(GPtrArray) refs = NULL;

    clawt_attachment_block_add(block, "a.png", "/h/a.png", "/g/a.png",
                               "1-a.png");
    clawt_attachment_block_add_unsaved(block, "b.png");
    clawt_attachment_block_add(block, "c.txt", "/h/c.txt", "/h/c.txt", NULL);
    body = g_string_free(block, FALSE);

    refs = clawt_attachment_refs_parse(body);
    g_assert_cmpuint(refs->len, ==, 2);
    g_assert_cmpstr(((ClawtAttachmentRef *)g_ptr_array_index(refs, 0))->value,
                    ==, "1-a.png");
    g_assert_cmpstr(((ClawtAttachmentRef *)g_ptr_array_index(refs, 1))->value,
                    ==, "/h/c.txt");
}

/* The kinds a tile is drawn by, from the name alone. */
static void
test_kinds_come_from_the_name(void)
{
    g_assert_cmpstr(clawt_attachment_kind("Shot.PNG"), ==, "image");
    g_assert_cmpstr(clawt_attachment_kind("paper.pdf"), ==, "pdf");
    g_assert_cmpstr(clawt_attachment_kind("notes.org"), ==, "text");
    g_assert_cmpstr(clawt_attachment_kind("build.log"), ==, "text");
    g_assert_cmpstr(clawt_attachment_kind("disk.iso"), ==, "other");
    g_assert_cmpstr(clawt_attachment_kind(NULL), ==, "other");
}

/* ── A conversation's files ─────────────────────────────────────────── */

/* A message as the room keeps it, with a timestamp of our choosing. */
static ClawtMessage *
message_at(const gchar *sender, const gchar *body, gint64 ts)
{
    ClawtMessage *message = clawt_message_new("dm:user:scout", sender, body);

    clawt_message_set_timestamp(message, ts);

    return message;
}

/*
 * Newest message first, block order within a message, the kinds from
 * the names, and a file deleted since it was sent listed as gone.
 *
 * What breaks without it: the gallery is the one place somebody looks
 * for "the PDF from last week", so the order is the feature; and a file
 * listed as there when it is not is a tile that opens nothing.
 */
static void
test_a_conversation_lists_its_files(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *store = NULL;
    g_autofree gchar *pdf_source = NULL;
    g_autofree gchar *pdf_id = NULL;
    g_autofree gchar *png_id = NULL;
    g_autofree gchar *png_path = NULL;
    g_autofree gchar *older = NULL;
    g_autofree gchar *newer = NULL;
    g_autoptr(GPtrArray) messages = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_message_free);
    g_autoptr(GPtrArray) items = NULL;
    ClawtAttachmentItem *item;

    fixture_setup(&fixture);
    store = g_build_filename(fixture.dir, "attachments", NULL);
    pdf_source = g_build_filename(fixture.dir, "paper.pdf", NULL);
    g_assert_true(g_file_set_contents(pdf_source, "%PDF-1.4", -1, NULL));

    png_id = clawt_attachment_store(store, fixture.source, NULL);
    pdf_id = clawt_attachment_store(store, pdf_source, NULL);
    g_assert_nonnull(png_id);
    g_assert_nonnull(pdf_id);

    older = g_strdup_printf("first\n\n" CLAWT_ATTACHMENT_MARKER ".\n"
                            "- screenshot.png\n  clawt:%s\n", png_id);
    newer = g_strdup_printf(CLAWT_ATTACHMENT_MARKER ".\n"
                            "- paper.pdf\n  /h/paper.pdf\n  clawt:%s\n"
                            "- old.txt\n  /nowhere/old.txt\n", pdf_id);

    g_ptr_array_add(messages, message_at("user", older, 1000));
    g_ptr_array_add(messages, message_at("scout", "no files here", 1500));
    g_ptr_array_add(messages, message_at("scout", newer, 2000));

    items = clawt_attachment_list(store, messages);
    g_assert_cmpuint(items->len, ==, 3);

    item = g_ptr_array_index(items, 0);
    g_assert_cmpstr(item->id, ==, pdf_id);
    g_assert_cmpstr(item->name, ==, "paper.pdf");
    g_assert_cmpstr(item->kind, ==, "pdf");
    g_assert_cmpstr(item->sender, ==, "scout");
    g_assert_cmpint(item->ts, ==, 2000);
    g_assert_true(item->exists);
    g_assert_cmpint(item->bytes, ==, 8);

    item = g_ptr_array_index(items, 1);
    g_assert_null(item->id);
    g_assert_cmpstr(item->path, ==, "/nowhere/old.txt");
    g_assert_cmpstr(item->kind, ==, "text");
    g_assert_false(item->exists);

    item = g_ptr_array_index(items, 2);
    g_assert_cmpstr(item->id, ==, png_id);
    g_assert_cmpstr(item->kind, ==, "image");
    g_assert_cmpstr(item->sender, ==, "user");
    g_assert_true(item->exists);

    /* deleted since: still listed, as gone */
    png_path = clawt_attachment_path(store, png_id);
    g_assert_cmpint(g_unlink(png_path), ==, 0);
    g_clear_pointer(&items, g_ptr_array_unref);
    items = clawt_attachment_list(store, messages);
    item = g_ptr_array_index(items, 2);
    g_assert_false(item->exists);
    g_assert_cmpint(item->bytes, ==, 0);

    fixture_teardown(&fixture);
}

/*
 * An id a message names that is not one of ours is listed as gone, not
 * resolved: a message body is text any agent can write.
 */
static void
test_a_hostile_id_in_a_message_resolves_to_nothing(void)
{
    g_autoptr(GPtrArray) messages = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_message_free);
    g_autoptr(GPtrArray) items = NULL;

    g_ptr_array_add(messages, message_at(
        "scout", CLAWT_ATTACHMENT_MARKER ".\n- x\n  clawt:../../etc/passwd\n",
        1));
    items = clawt_attachment_list("/tmp/store", messages);

    g_assert_cmpuint(items->len, ==, 1);
    g_assert_false(((ClawtAttachmentItem *)
                    g_ptr_array_index(items, 0))->exists);
}

/* ── Thumbnails ─────────────────────────────────────────────────────── */

/* The fixture PDF stored as an attachment in @store. */
static gchar *
store_fixture_pdf(const gchar *store)
{
    g_autofree gchar *source = g_build_filename(CLAWT_TEST_FIXTURES,
                                                "attachment-page.pdf", NULL);
    g_autoptr(GError) error = NULL;
    gchar *id = clawt_attachment_store(store, source, &error);

    g_assert_no_error(error);
    return id;
}

/*
 * A PDF's first page is a PNG whose longer edge is the size asked for:
 * the fixture is a 200x100 page, so 64 gives 64x32.  Asked again, the
 * cached copy answers with the same bytes.
 *
 * What breaks without it: the Files gallery's PDF tiles, in every
 * client.  Skipped in a build without poppler, where the refusal is
 * what is checked instead.
 */
static void
test_a_pdf_thumbnail_is_its_first_page(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *store = NULL;
    g_autofree gchar *id = NULL;
    g_autofree gchar *cached = NULL;
    g_autofree gchar *leaf = NULL;
    g_autoptr(GBytes) png = NULL;
    g_autoptr(GBytes) again = NULL;
    g_autoptr(GError) error = NULL;
    guint width = 0;
    guint height = 0;
    gsize length = 0;
    const guint8 *data;

    fixture_setup(&fixture);
    store = g_build_filename(fixture.dir, "attachments", NULL);
    id = store_fixture_pdf(store);

    png = clawt_attachment_thumbnail(store, id, 64, &width, &height, &error);

    if (!clawt_attachment_can_render_pdf()) {
        g_assert_null(png);
        g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED);
        fixture_teardown(&fixture);
        g_test_skip("built without poppler-glib");
        return;
    }

    g_assert_no_error(error);
    g_assert_nonnull(png);
    g_assert_cmpuint(width, ==, 64);
    g_assert_cmpuint(height, ==, 32);

    data = g_bytes_get_data(png, &length);
    g_assert_cmpuint(length, >, 24);
    g_assert_cmpint(memcmp(data, "\211PNG\r\n\032\n", 8), ==, 0);

    /* cached under .thumbs, and served from there with its size */
    leaf = g_strdup_printf("%s.64.png", id);
    cached = g_build_filename(store, ".thumbs", leaf, NULL);
    g_assert_true(g_file_test(cached, G_FILE_TEST_IS_REGULAR));

    width = height = 0;
    again = clawt_attachment_thumbnail(store, id, 64, &width, &height, NULL);
    g_assert_true(g_bytes_equal(png, again));
    g_assert_cmpuint(width, ==, 64);
    g_assert_cmpuint(height, ==, 32);

    fixture_teardown(&fixture);
}

/*
 * The size off the wire is clamped: a request cannot ask the daemon for
 * a surface of its own choosing, and 0 means the default.
 */
static void
test_the_thumbnail_size_is_clamped(void)
{
    g_assert_cmpuint(clawt_attachment_thumbnail_size(0), ==,
                     CLAWT_ATTACHMENT_THUMB_DEFAULT);
    g_assert_cmpuint(clawt_attachment_thumbnail_size(-5), ==,
                     CLAWT_ATTACHMENT_THUMB_DEFAULT);
    g_assert_cmpuint(clawt_attachment_thumbnail_size(1), ==,
                     CLAWT_ATTACHMENT_THUMB_MIN);
    g_assert_cmpuint(clawt_attachment_thumbnail_size(100000), ==,
                     CLAWT_ATTACHMENT_THUMB_MAX);
    g_assert_cmpuint(clawt_attachment_thumbnail_size(128), ==, 128);
}

/*
 * A text file's tile is its first lines, cut, and valid UTF-8 whatever
 * the file held.
 */
static void
test_a_text_thumbnail_is_its_first_lines(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *store = NULL;
    g_autofree gchar *source = NULL;
    g_autofree gchar *id = NULL;
    g_autofree gchar *head = NULL;

    fixture_setup(&fixture);
    store = g_build_filename(fixture.dir, "attachments", NULL);
    source = g_build_filename(fixture.dir, "build.log", NULL);
    g_assert_true(g_file_set_contents(
        source, "one\ntwo\r\nthree \xff\nfour\nfive\n", -1, NULL));
    id = clawt_attachment_store(store, source, NULL);

    head = clawt_attachment_text_head(store, id, 3, NULL);
    g_assert_nonnull(head);
    g_assert_true(g_utf8_validate(head, -1, NULL));
    g_assert_true(g_str_has_prefix(head, "one\ntwo\nthree "));
    g_assert_null(strstr(head, "four"));

    fixture_teardown(&fixture);
}

/*
 * An unknown id is not found, and a hostile one is refused before any
 * file is opened -- for the picture and for the text alike.
 *
 * What breaks without it: attachment.thumbnail takes its id off the
 * wire, and `../x` resolved would draw, or print the first lines of, a
 * file the daemon was never asked to serve.
 */
static void
test_a_thumbnail_id_is_checked(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *store = NULL;
    g_autoptr(GError) unknown = NULL;
    g_autoptr(GError) hostile = NULL;
    g_autoptr(GError) hostile_text = NULL;
    g_autoptr(GBytes) png = NULL;
    g_autoptr(GBytes) escaped = NULL;
    g_autofree gchar *text = NULL;

    fixture_setup(&fixture);
    store = g_build_filename(fixture.dir, "attachments", NULL);

    png = clawt_attachment_thumbnail(store, "0abc-missing.pdf", 64, NULL,
                                     NULL, &unknown);
    g_assert_null(png);
    g_assert_error(unknown, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND);

    escaped = clawt_attachment_thumbnail(store, "../x.pdf", 64, NULL, NULL,
                                         &hostile);
    g_assert_null(escaped);
    g_assert_error(hostile, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);

    text = clawt_attachment_text_head(store, "../x.txt", 3, &hostile_text);
    g_assert_null(text);
    g_assert_error(hostile_text, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);

    fixture_teardown(&fixture);
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
    g_test_add_func("/attachment/refs/every-shape", test_refs_read_every_shape);
    g_test_add_func("/attachment/refs/builder-round-trip",
                    test_refs_read_what_the_builder_wrote);
    g_test_add_func("/attachment/kinds", test_kinds_come_from_the_name);
    g_test_add_func("/attachment/list/order-kinds-gone",
                    test_a_conversation_lists_its_files);
    g_test_add_func("/attachment/list/hostile-id",
                    test_a_hostile_id_in_a_message_resolves_to_nothing);
    g_test_add_func("/attachment/thumbnail/pdf-first-page",
                    test_a_pdf_thumbnail_is_its_first_page);
    g_test_add_func("/attachment/thumbnail/size-clamped",
                    test_the_thumbnail_size_is_clamped);
    g_test_add_func("/attachment/thumbnail/text-first-lines",
                    test_a_text_thumbnail_is_its_first_lines);
    g_test_add_func("/attachment/thumbnail/id-checked",
                    test_a_thumbnail_id_is_checked);

    return g_test_run();
}
