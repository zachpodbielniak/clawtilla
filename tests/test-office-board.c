/*
 * test-office-board.c - The office's usage board: its words and its layout
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The board clawtilla-office draws and the panel it opens are laid out
 * by libclawt over a text-measuring callback, so these tests run the
 * same arithmetic with a font of known widths.  What they hold it to is
 * what went wrong on the first board: text wider than its column, a
 * board spilling out of its zone over the walls, a panel off the edge
 * of the view, and logins and windows dropped without a word.
 */

#include "clawtilla.h"

#include <string.h>

/* every character this wide, times the size: a monospaced test font */
static gfloat
measure_mono(const gchar *text, gfloat size, gpointer user_data)
{
    gfloat per = user_data != NULL ? *(gfloat *)user_data : 0.6f;

    return (gfloat)g_utf8_strlen(text, -1) * size * per;
}

static JsonObject *
load_fixture(void)
{
    g_autofree gchar *path = g_build_filename(CLAWT_TEST_FIXTURES, "office",
                                              "usage-board.json", NULL);
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(GError) error = NULL;

    g_assert_true(json_parser_load_from_file(parser, path, &error));
    g_assert_no_error(error);
    return json_object_ref(json_node_get_object(json_parser_get_root(parser)));
}

/* A usage answer with @n logins of one window each, for crowding the wall. */
static JsonObject *
many_logins(guint n)
{
    g_autoptr(JsonBuilder) b = json_builder_new();
    g_autoptr(JsonNode) root = NULL;
    guint i;

    json_builder_begin_object(b);
    json_builder_set_member_name(b, "logins");
    json_builder_begin_array(b);
    for (i = 0; i < n; i++) {
        g_autofree gchar *account = g_strdup_printf("account-%u", i);

        json_builder_begin_object(b);
        json_builder_set_member_name(b, "provider");
        json_builder_add_string_value(b, "claude-code");
        json_builder_set_member_name(b, "account");
        json_builder_add_string_value(b, account);
        json_builder_set_member_name(b, "state");
        json_builder_add_string_value(b, "ok");
        json_builder_set_member_name(b, "windows");
        json_builder_begin_array(b);
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "label");
        json_builder_add_string_value(b, "Weekly");
        json_builder_set_member_name(b, "kind");
        json_builder_add_string_value(b, "meter");
        json_builder_set_member_name(b, "unit");
        json_builder_add_string_value(b, "percent");
        json_builder_set_member_name(b, "remaining_percent");
        json_builder_add_double_value(b, (gdouble)(i * 9 % 100));
        json_builder_end_object(b);
        json_builder_end_array(b);
        json_builder_end_object(b);
    }
    json_builder_end_array(b);
    json_builder_end_object(b);
    root = json_builder_get_root(b);
    return json_object_ref(json_node_get_object(root));
}

/* ── The words ────────────────────────────────────────────────────── */

/*
 * Every login and every window, worded by the calls the Fleet page
 * makes -- the old board stopped at four logins and two windows each and
 * drew a balance as an empty meter.
 */
static void
test_model_carries_everything(void)
{
    g_autoptr(JsonObject) usage = load_fixture();
    gint64 now = json_object_get_int_member(usage, "now");
    g_autoptr(ClawtOfficeBoard) board = clawt_office_board_new(usage, now);
    JsonArray *logins = json_object_get_array_member(usage, "logins");
    ClawtOfficeBoardLogin *claude;
    ClawtOfficeBoardLogin *grok;
    ClawtOfficeBoardLogin *gemini;
    ClawtOfficeBoardWindow *balance;
    guint i;
    guint w;

    g_assert_cmpuint(board->logins->len, ==, json_array_get_length(logins));
    g_assert_cmpuint(clawt_office_board_n_windows(board), ==, 7);

    for (i = 0; i < board->logins->len; i++) {
        ClawtOfficeBoardLogin *login = g_ptr_array_index(board->logins, i);
        JsonObject *raw = json_array_get_object_element(logins, i);
        g_autofree gchar *title = clawt_usage_login_title(raw);
        g_autofree gchar *status = clawt_usage_login_status_label(raw, now);
        JsonArray *windows = json_object_has_member(raw, "windows")
            ? json_object_get_array_member(raw, "windows") : NULL;

        g_assert_cmpstr(login->title, ==, title);
        g_assert_cmpstr(login->status, ==, status);
        g_assert_cmpstr(login->tone, ==,
                        clawt_allowance_level_tone(clawt_usage_login_level(raw)));
        for (w = 0; windows != NULL && w < json_array_get_length(windows); w++) {
            ClawtOfficeBoardWindow *window = g_ptr_array_index(login->windows, w);
            JsonObject *raw_window = json_array_get_object_element(windows, w);
            g_autofree gchar *value = clawt_usage_window_value_label(raw_window);
            g_autofree gchar *reset = clawt_usage_window_reset_label(raw_window, now);

            g_assert_cmpstr(window->value, ==, value);
            g_assert_cmpstr(window->reset, ==, reset);
            g_assert_cmpstr(window->tone, ==, clawt_allowance_level_tone(
                clawt_usage_window_level(raw_window)));
        }
    }

    /* the tightest is the shared helper's: 8% left of the Opus week */
    claude = g_ptr_array_index(board->logins, 0);
    g_assert_cmpstr(claude->initial, ==, "C");
    g_assert_cmpuint(claude->windows->len, ==, 4);
    g_assert_cmpint(claude->tightest, ==, 2);
    g_assert_cmpstr(claude->tone, ==, "bad");

    /* a balance is a figure: no meter, no warning, never "% left" */
    grok = g_ptr_array_index(board->logins, 2);
    balance = g_ptr_array_index(grok->windows, 0);
    g_assert_true(balance->balance);
    g_assert_false(balance->has_left);
    g_assert_cmpstr(balance->value, ==, "$0.00 prepaid");
    g_assert_cmpstr(balance->tone, ==, "neutral");
    g_assert_cmpint(grok->tightest, ==, 1);

    /* a login with no windows says why, rather than drawing an empty meter */
    gemini = g_ptr_array_index(board->logins, 3);
    g_assert_cmpuint(gemini->windows->len, ==, 0);
    g_assert_cmpint(gemini->tightest, ==, -1);
    g_assert_nonnull(strstr(gemini->status, "Reports no plan limits"));
}

/* The web client's copy: the same words, a kind, and no share for a balance. */
static void
test_model_to_json(void)
{
    g_autoptr(JsonObject) usage = load_fixture();
    g_autoptr(ClawtOfficeBoard) board = clawt_office_board_new(usage,
        json_object_get_int_member(usage, "now"));
    g_autoptr(JsonObject) json = clawt_office_board_to_json(board);
    JsonArray *logins = json_object_get_array_member(json, "logins");
    JsonObject *claude = json_array_get_object_element(logins, 0);
    JsonObject *grok = json_array_get_object_element(logins, 2);
    JsonObject *prepaid = json_array_get_object_element(
        json_object_get_array_member(grok, "windows"), 0);
    JsonObject *session = json_array_get_object_element(
        json_object_get_array_member(claude, "windows"), 0);

    g_assert_cmpuint(json_array_get_length(logins), ==, 4);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(claude, "windows")),
                     ==, 4);
    g_assert_cmpint(json_object_get_int_member(claude, "tightest"), ==, 2);
    g_assert_cmpstr(json_object_get_string_member(claude, "initial"), ==, "C");
    g_assert_cmpstr(json_object_get_string_member(session, "kind"), ==, "meter");
    g_assert_cmpstr(json_object_get_string_member(session, "value"), ==, "62% left");
    g_assert_cmpstr(json_object_get_string_member(session, "reset"), ==, "resets Tue 9am");
    g_assert_cmpfloat(json_object_get_double_member(session, "remaining"), ==, 62.0);
    g_assert_cmpstr(json_object_get_string_member(prepaid, "kind"), ==, "balance");
    g_assert_false(json_object_has_member(prepaid, "remaining"));
    g_assert_false(json_object_has_member(prepaid, "reset"));

    /* nothing at all is an empty board, not a crash */
    {
        g_autoptr(ClawtOfficeBoard) none = clawt_office_board_new(NULL, 0);
        g_autoptr(JsonObject) empty = clawt_office_board_to_json(none);

        g_assert_cmpuint(none->logins->len, ==, 0);
        g_assert_cmpuint(json_array_get_length(json_object_get_array_member(empty, "logins")),
                         ==, 0);
    }
}

/* ── Fitting text ─────────────────────────────────────────────────── */

static void
test_ellipsize(void)
{
    /* half the size a character: exact in binary, so no width is off by a rounding */
    gfloat per = 0.5f;
    g_autofree gchar *fits = clawt_office_board_ellipsize("Weekly", 50.0f, 10.0f,
                                                          measure_mono, &per);
    g_autofree gchar *cut = clawt_office_board_ellipsize("Weekly (Opus, a longer name)", 50.0f,
                                                         10.0f, measure_mono, &per);
    g_autofree gchar *nothing = clawt_office_board_ellipsize("Weekly", 4.0f, 10.0f,
                                                             measure_mono, &per);
    g_autofree gchar *wide = clawt_office_board_ellipsize("\xc3\xa9t\xc3\xa9 caf\xc3\xa9", 25.0f,
                                                          10.0f, measure_mono, &per);

    g_assert_cmpstr(fits, ==, "Weekly");
    /* ten characters of room: nine and the ellipsis */
    g_assert_cmpstr(cut, ==, "Weekly (O\xe2\x80\xa6");
    g_assert_cmpfloat(measure_mono(cut, 10.0f, &per), <=, 50.0f);
    g_assert_cmpstr(nothing, ==, "");
    /* cut at a character, never inside one, and no space left before the ellipsis */
    g_assert_cmpstr(wide, ==, "\xc3\xa9t\xc3\xa9\xe2\x80\xa6");
    g_assert_true(g_utf8_validate(wide, -1, NULL));
}

/* ── Checking a layout ────────────────────────────────────────────── */

/* Every item inside the rect, and every text within its column. */
static void
assert_inside(ClawtOfficeBoardLayout *layout, gfloat x, gfloat y, gfloat width, gfloat height,
              gfloat min_text, ClawtOfficeBoardMeasure measure, gpointer user_data)
{
    guint i;

    for (i = 0; i < layout->items->len; i++) {
        ClawtOfficeBoardItem *item = &g_array_index(layout->items, ClawtOfficeBoardItem, i);

        g_assert_cmpfloat(item->x, >=, x - 0.01f);
        g_assert_cmpfloat(item->y, >=, y - 0.01f);
        g_assert_cmpfloat(item->x + item->width, <=, x + width + 0.01f);
        g_assert_cmpfloat(item->y + item->height, <=, y + height + 0.01f);
        if (item->kind == CLAWT_OFFICE_BOARD_ITEM_TEXT) {
            g_assert_nonnull(item->text);
            g_assert_cmpfloat(item->width, <=, item->room + 0.01f);
            g_assert_cmpfloat(measure(item->text, item->size, user_data), <=,
                              item->room + 0.01f);
            g_assert_cmpfloat(item->size, >=, min_text);
        }
    }
}

static guint
count_kind(ClawtOfficeBoardLayout *layout, ClawtOfficeBoardItemKind kind)
{
    guint n = 0;
    guint i;

    for (i = 0; i < layout->items->len; i++)
        if (g_array_index(layout->items, ClawtOfficeBoardItem, i).kind == kind)
            n++;
    return n;
}

static gboolean
has_text(ClawtOfficeBoardLayout *layout, const gchar *text)
{
    guint i;

    for (i = 0; i < layout->items->len; i++) {
        ClawtOfficeBoardItem *item = &g_array_index(layout->items, ClawtOfficeBoardItem, i);

        if (item->kind == CLAWT_OFFICE_BOARD_ITEM_TEXT && g_strcmp0(item->text, text) == 0)
            return TRUE;
    }
    return FALSE;
}

/* ── On the wall ──────────────────────────────────────────────────── */

/*
 * The two shipped zones -- modern-office's 160x160 board and
 * cozy-town's 288x160 square -- at the zooms the Office page offers
 * (Fit at 1280x720 is 0.5625 for modern-office), at device scales 1 and
 * 2: nothing ever leaves the zone, no text is wider than its column or
 * smaller than the pixel font, words where they fit and gauges where
 * they do not.
 */
static void
test_world_stays_in_zone(void)
{
    static const gfloat zones[][2] = { { 160.0f, 160.0f }, { 288.0f, 160.0f } };
    static const gfloat zooms[] = { 0.5f, 0.5625f, 1.0f, 2.0f, 4.0f };
    gfloat per = 0.6f;
    g_autoptr(JsonObject) usage = load_fixture();
    g_autoptr(ClawtOfficeBoard) board = clawt_office_board_new(usage,
        json_object_get_int_member(usage, "now"));
    guint z;
    guint k;
    gint ui;

    for (z = 0; z < G_N_ELEMENTS(zones); z++)
        for (k = 0; k < G_N_ELEMENTS(zooms); k++)
            for (ui = 1; ui <= 2; ui++) {
                gfloat zoom = zooms[k] * (gfloat)ui;
                gfloat zw = zones[z][0] * zoom;
                gfloat zh = zones[z][1] * zoom;
                g_autoptr(ClawtOfficeBoardLayout) layout = clawt_office_board_layout_world(
                    board, 37.0f, 51.0f, zw, zh, ui, measure_mono, &per);

                g_assert_cmpint(layout->detail, !=, CLAWT_OFFICE_BOARD_DETAIL_NONE);
                assert_inside(layout, 37.0f, 51.0f, zw, zh,
                              CLAWT_OFFICE_BOARD_MIN_TEXT * (gfloat)ui, measure_mono, &per);
                if (layout->detail == CLAWT_OFFICE_BOARD_DETAIL_ROWS) {
                    /* every window's label is there, cut or whole */
                    g_assert_cmpuint(count_kind(layout, CLAWT_OFFICE_BOARD_ITEM_TEXT), >=,
                                     board->logins->len + clawt_office_board_n_windows(board));
                    /* and a balance has no meter: 7 windows, 6 meters */
                    g_assert_cmpuint(count_kind(layout, CLAWT_OFFICE_BOARD_ITEM_TRACK), ==, 6);
                }
                if (layout->detail == CLAWT_OFFICE_BOARD_DETAIL_GAUGES)
                    g_assert_cmpuint(count_kind(layout, CLAWT_OFFICE_BOARD_ITEM_BADGE) +
                                     layout->hidden, ==, board->logins->len);
            }
}

/* The level of detail the shipped board gets at the zooms people use. */
static void
test_world_detail_by_size(void)
{
    gfloat per = 0.6f;
    g_autoptr(JsonObject) usage = load_fixture();
    g_autoptr(ClawtOfficeBoard) board = clawt_office_board_new(usage,
        json_object_get_int_member(usage, "now"));
    g_autoptr(ClawtOfficeBoardLayout) fit = clawt_office_board_layout_world(
        board, 0.0f, 0.0f, 90.0f, 90.0f, 1, measure_mono, &per);
    g_autoptr(ClawtOfficeBoardLayout) big = clawt_office_board_layout_world(
        board, 0.0f, 0.0f, 320.0f, 320.0f, 1, measure_mono, &per);
    g_autoptr(ClawtOfficeBoardLayout) dot = clawt_office_board_layout_world(
        board, 0.0f, 0.0f, 12.0f, 12.0f, 1, measure_mono, &per);
    g_autoptr(ClawtOfficeBoardLayout) none = clawt_office_board_layout_world(
        board, 0.0f, 0.0f, 0.0f, 40.0f, 1, measure_mono, &per);

    /* at Fit: a gauge per login, every one of them */
    g_assert_cmpint(fit->detail, ==, CLAWT_OFFICE_BOARD_DETAIL_GAUGES);
    g_assert_cmpuint(fit->hidden, ==, 0);
    g_assert_cmpuint(count_kind(fit, CLAWT_OFFICE_BOARD_ITEM_BADGE), ==, 4);
    g_assert_true(has_text(fit, "C"));
    /* at 2x: words */
    g_assert_cmpint(big->detail, ==, CLAWT_OFFICE_BOARD_DETAIL_ROWS);
    g_assert_true(has_text(big, "$0.00 prepaid"));
    /* far away: one mark in the worst tone */
    g_assert_cmpint(dot->detail, ==, CLAWT_OFFICE_BOARD_DETAIL_MARK);
    g_assert_cmpstr(g_array_index(dot->items, ClawtOfficeBoardItem, 0).tone, ==, "bad");
    g_assert_cmpint(none->detail, ==, CLAWT_OFFICE_BOARD_DETAIL_NONE);
    g_assert_cmpuint(none->items->len, ==, 0);
}

/* Too many logins for the wall: the rest are counted, never silently dropped. */
static void
test_world_counts_the_rest(void)
{
    gfloat per = 0.6f;
    g_autoptr(JsonObject) usage = many_logins(30);
    g_autoptr(ClawtOfficeBoard) board = clawt_office_board_new(usage, 0);
    g_autoptr(ClawtOfficeBoardLayout) layout = clawt_office_board_layout_world(
        board, 10.0f, 10.0f, 90.0f, 90.0f, 1, measure_mono, &per);
    g_autofree gchar *more = NULL;

    g_assert_cmpint(layout->detail, ==, CLAWT_OFFICE_BOARD_DETAIL_GAUGES);
    g_assert_cmpuint(layout->hidden, >, 0);
    g_assert_cmpuint(count_kind(layout, CLAWT_OFFICE_BOARD_ITEM_BADGE) + layout->hidden, ==,
                     30);
    more = g_strdup_printf("+%u", layout->hidden);
    g_assert_true(has_text(layout, more));
    assert_inside(layout, 10.0f, 10.0f, 90.0f, 90.0f, CLAWT_OFFICE_BOARD_MIN_TEXT,
                  measure_mono, &per);
}

/* ── The details panel ────────────────────────────────────────────── */

/*
 * Beside the board, inside the view, in a roomy view and a cramped one,
 * from a board at either edge, and docked: every text within its column,
 * the panel within the view, every window listed when it fits and
 * counted when it does not.
 */
static void
test_panel_in_view(void)
{
    static const gfloat views[][2] = { { 1280.0f, 720.0f }, { 640.0f, 360.0f },
                                       { 320.0f, 200.0f }, { 2560.0f, 1440.0f } };
    static const gfloat anchors[][2] = { { 1000.0f, 400.0f }, { 4.0f, 4.0f },
                                         { 200.0f, 150.0f } };
    gfloat per = 0.6f;
    g_autoptr(JsonObject) usage = load_fixture();
    g_autoptr(ClawtOfficeBoard) board = clawt_office_board_new(usage,
        json_object_get_int_member(usage, "now"));
    guint v;
    guint a;
    gint docked;
    gint ui;

    for (v = 0; v < G_N_ELEMENTS(views); v++)
        for (a = 0; a < G_N_ELEMENTS(anchors); a++)
            for (docked = 0; docked <= 1; docked++)
                for (ui = 1; ui <= 2; ui++) {
                    gfloat vw = views[v][0];
                    gfloat vh = views[v][1];
                    g_autoptr(ClawtOfficeBoardLayout) panel = clawt_office_board_layout_panel(
                        board, docked, MIN(anchors[a][0], vw - 40.0f),
                        MIN(anchors[a][1], vh - 40.0f), 40.0f, 40.0f, vw, vh, ui,
                        measure_mono, &per);
                    guint windows = 0;
                    guint i;

                    g_assert_cmpint(panel->detail, ==, CLAWT_OFFICE_BOARD_DETAIL_PANEL);
                    g_assert_cmpfloat(panel->x, >=, 0.0f);
                    g_assert_cmpfloat(panel->y, >=, 0.0f);
                    g_assert_cmpfloat(panel->x + panel->width, <=, vw);
                    g_assert_cmpfloat(panel->y + panel->height, <=, vh);
                    assert_inside(panel, panel->x, panel->y, panel->width, panel->height,
                                  CLAWT_OFFICE_BOARD_PANEL_TEXT * (gfloat)ui, measure_mono,
                                  &per);

                    /* every window drawn, or counted in the "+N more" row */
                    for (i = 0; i < panel->items->len; i++) {
                        ClawtOfficeBoardItem *item = &g_array_index(panel->items,
                                                                    ClawtOfficeBoardItem, i);

                        if (item->kind == CLAWT_OFFICE_BOARD_ITEM_TEXT &&
                            item->ink == CLAWT_OFFICE_BOARD_INK_LABEL)
                            windows++;
                    }
                    if (panel->hidden > 0) {
                        g_autofree gchar *more = g_strdup_printf("+%u more", panel->hidden);

                        g_assert_true(has_text(panel, more));
                    }
                    /* the windows not shown, plus the login with none (gemini) when hidden */
                    g_assert_cmpuint(windows + panel->hidden, >=,
                                     clawt_office_board_n_windows(board));
                    g_assert_cmpuint(windows + panel->hidden, <=,
                                     clawt_office_board_n_windows(board) + 1);
                    if (docked)
                        /* top right */
                        g_assert_cmpfloat(panel->x + panel->width, >=,
                                          vw - 8.0f * (gfloat)ui - 1.0f);
                }
}

/*
 * The roomy case in detail: everything there, the reset column, no meter
 * for the balance, and beside the board rather than over it.
 */
static void
test_panel_roomy(void)
{
    gfloat per = 0.6f;
    g_autoptr(JsonObject) usage = load_fixture();
    g_autoptr(ClawtOfficeBoard) board = clawt_office_board_new(usage,
        json_object_get_int_member(usage, "now"));
    g_autoptr(ClawtOfficeBoardLayout) panel = clawt_office_board_layout_panel(
        board, FALSE, 300.0f, 200.0f, 90.0f, 90.0f, 1920.0f, 1080.0f, 1, measure_mono, &per);
    g_autoptr(ClawtOfficeBoardLayout) left = clawt_office_board_layout_panel(
        board, FALSE, 1800.0f, 200.0f, 90.0f, 90.0f, 1920.0f, 1080.0f, 1, measure_mono, &per);

    g_assert_cmpuint(panel->hidden, ==, 0);
    g_assert_true(has_text(panel, "Provider usage"));
    g_assert_true(has_text(panel, "resets Tue 9am"));
    g_assert_true(has_text(panel, "$0.00 prepaid"));
    g_assert_true(has_text(panel, "Session (5h)"));
    /* 7 windows, one a balance: 6 meters */
    g_assert_cmpuint(count_kind(panel, CLAWT_OFFICE_BOARD_ITEM_TRACK), ==, 6);
    /* right of the board when there is room; left of it at the right edge */
    g_assert_cmpfloat(panel->x, >=, 390.0f);
    g_assert_cmpfloat(left->x + left->width, <=, 1800.0f);
}

gint
main(gint argc, gchar *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/office-board/model/everything", test_model_carries_everything);
    g_test_add_func("/office-board/model/json", test_model_to_json);
    g_test_add_func("/office-board/ellipsize", test_ellipsize);
    g_test_add_func("/office-board/world/in-zone", test_world_stays_in_zone);
    g_test_add_func("/office-board/world/detail", test_world_detail_by_size);
    g_test_add_func("/office-board/world/counts-the-rest", test_world_counts_the_rest);
    g_test_add_func("/office-board/panel/in-view", test_panel_in_view);
    g_test_add_func("/office-board/panel/roomy", test_panel_roomy);

    return g_test_run();
}
