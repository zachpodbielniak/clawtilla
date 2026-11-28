/*
 * test-office-face.c - Which face a person in the office wears
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The decision the native scene and the web office both call.  It needs
 * no window: a picture wins over the generated face, a character sheet
 * wins over the picture, and a Grok bot or a visitor -- no face, no
 * picture -- gets the generated face of its id.  The PNG decode is the
 * one part that needs cairo (POPPLER=1); without it that test skips and
 * says why, and the decision tests still run.
 */

#include "clawtilla.h"

#include <string.h>

/* 4x4 PNG, every pixel RGB 255,0,17. */
static const guint8 RED_PNG[] = {
    137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,4,0,0,0,4,8,2,0,0,0,38,147,9,41,
    0,0,0,16,73,68,65,84,120,218,99,248,207,32,8,71,12,196,113,0,201,35,17,1,6,239,147,43,
    0,0,0,0,73,69,78,68,174,66,96,130
};

static void
resolve(ClawtOfficeFace *face, const gchar *id, const gchar *name,
        const gchar *color_in, const gchar *shape, gboolean has_avatar,
        gboolean has_character, gchar *color, gchar *initial)
{
    clawt_office_face_resolve(face, id, name, color_in, shape, has_avatar,
                              has_character, color, 16, initial, 8);
}

/*
 * The order the office draws: a sheet, then a picture, then the
 * generated face, then the letter disc.  This is what fails on
 * 9de0b7de, which never read has_avatar.
 */
static void
test_order(void)
{
    ClawtOfficeFace face;
    gchar color[16];
    gchar initial[8];
    const gchar *ada_color;

    resolve(&face, "ada", "Ada", "#7c5cff", NULL, TRUE, TRUE, color, initial);
    g_assert_cmpint(face.kind, ==, CLAWT_OFFICE_FACE_CHARACTER);
    g_assert_true(face.has_picture);

    resolve(&face, "ada", "Ada", "#7c5cff", NULL, TRUE, FALSE, color, initial);
    g_assert_cmpint(face.kind, ==, CLAWT_OFFICE_FACE_PICTURE);
    g_assert_true(face.has_picture);
    ada_color = clawt_avatar_color("ada", "Ada", "#7c5cff");
    g_assert_cmpstr(color, ==, ada_color);
    g_assert_cmpstr(initial, ==, "A");

    resolve(&face, "ada", "Ada", NULL, NULL, FALSE, FALSE, color, initial);
    g_assert_cmpint(face.kind, ==, CLAWT_OFFICE_FACE_GENERATED);
    g_assert_false(face.has_picture);
    g_assert_cmpint(face.shape, ==, clawt_face_resolve("ada", NULL));
    g_assert_cmpstr(color, ==, clawt_avatar_color("ada", "Ada", NULL));

    resolve(&face, "ada", "Ada", NULL, "", FALSE, FALSE, color, initial);
    g_assert_cmpint(face.kind, ==, CLAWT_OFFICE_FACE_GENERATED);

    resolve(&face, "ada", "Ada", NULL, "initials", FALSE, FALSE, color, initial);
    g_assert_cmpint(face.kind, ==, CLAWT_OFFICE_FACE_BLOB);
    g_assert_cmpint(face.shape, ==, CLAWT_FACE_INITIALS);

    resolve(&face, "ada", "Ada", NULL, "not-a-shape", FALSE, FALSE, color, initial);
    g_assert_cmpint(face.kind, ==, CLAWT_OFFICE_FACE_BLOB);

    /* a picture beats the letter disc; the sheet still beats the picture */
    resolve(&face, "ada", "Ada", NULL, "initials", TRUE, FALSE, color, initial);
    g_assert_cmpint(face.kind, ==, CLAWT_OFFICE_FACE_PICTURE);
}

/* Not in agent.list: no face, no picture, the generated face of the id. */
static void
test_visitor_and_grok(void)
{
    ClawtOfficeFace face;
    gchar color[16];
    gchar initial[8];

    resolve(&face, "studio:ada", "Ada", "#112233", NULL, FALSE, FALSE, color, initial);
    g_assert_cmpint(face.kind, ==, CLAWT_OFFICE_FACE_GENERATED);
    g_assert_false(face.has_picture);
    g_assert_cmpint(face.shape, ==, clawt_face_resolve("studio:ada", NULL));
    g_assert_cmpstr(color, ==, clawt_avatar_color("studio:ada", "Ada", "#112233"));
    g_assert_cmpstr(initial, ==, "A");

    resolve(&face, "grok:chief", "Chief", NULL, NULL, FALSE, FALSE, color, initial);
    g_assert_cmpint(face.kind, ==, CLAWT_OFFICE_FACE_GENERATED);
    g_assert_cmpint(face.shape, ==, clawt_face_resolve("grok:chief", NULL));
    g_assert_cmpstr(initial, ==, "C");
}

static void
test_picture_url(void)
{
    g_autofree gchar *plain = clawt_office_face_picture_url("ada");
    g_autofree gchar *slash = clawt_office_face_picture_url("a/b");
    g_autofree gchar *empty = clawt_office_face_picture_url("");
    g_autofree gchar *missing = clawt_office_face_picture_url(NULL);

    g_assert_cmpstr(plain, ==, "/a/ada/avatar");
    g_assert_cmpstr(slash, ==, "/a/a%2Fb/avatar");
    g_assert_null(empty);
    g_assert_null(missing);
}

static void
test_decode_png(void)
{
    g_autofree guint8 *rgba = NULL;
    gint size = 0;
    g_autoptr(GError) error = NULL;
    guint8 junk[] = { 'n', 'o', 't', ' ', 'a', ' ', 'p', 'n', 'g' };
    gint center;

    if (!clawt_office_portrait_decode_available()) {
        g_test_skip("cairo PNG decode is not in this build (POPPLER=0); "
                    "gdk-pixbuf is not used because glycin's bwrap sandbox "
                    "does not run in these containers");
        return;
    }

    g_assert_true(clawt_office_portrait_decode(RED_PNG, sizeof(RED_PNG), &rgba, &size,
                                               &error));
    g_assert_no_error(error);
    g_assert_cmpint(size, ==, CLAWT_OFFICE_PORTRAIT_SIZE);
    g_assert_nonnull(rgba);
    g_assert_cmpint(rgba[3], ==, 0);
    center = (size / 2) * size + (size / 2);
    g_assert_cmpint(rgba[center * 4 + 0], ==, 255);
    g_assert_cmpint(rgba[center * 4 + 1], ==, 0);
    g_assert_cmpint(rgba[center * 4 + 2], ==, 17);
    g_assert_cmpint(rgba[center * 4 + 3], ==, 255);

    g_assert_false(clawt_office_portrait_decode(junk, sizeof(junk), &rgba, &size, &error));
    g_assert_nonnull(error);
    g_assert_cmpint(error->code, ==, CLAWT_ERROR_INVALID_ARGUMENT);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/office-face/order", test_order);
    g_test_add_func("/office-face/visitor-and-grok", test_visitor_and_grok);
    g_test_add_func("/office-face/picture-url", test_picture_url);
    g_test_add_func("/office-face/decode-png", test_decode_png);

    return g_test_run();
}
