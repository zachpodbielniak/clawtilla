/*
 * clawt-office-board.c - The office's provider usage board, laid out
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The model is clawt_usage_*() and nothing else: every word, figure and
 * tone the Fleet page shows, for every login and every window.  The
 * layouts are arithmetic over a text-measuring callback, so the same
 * code places the board for raylib's pixel font in clawtilla-office and
 * for a fake font in the tests -- and a test can say "no text is wider
 * than its column, and nothing leaves the zone" about the real thing.
 */

#include "clawtilla.h"

#include <math.h>
#include <string.h>

/* "…", what an ellipsized text ends with */
#define ELLIPSIS "\xe2\x80\xa6"

G_DEFINE_BOXED_TYPE(ClawtOfficeBoard, clawt_office_board, clawt_office_board_ref,
                    clawt_office_board_free)
G_DEFINE_BOXED_TYPE(ClawtOfficeBoardLayout, clawt_office_board_layout,
                    clawt_office_board_layout_ref, clawt_office_board_layout_free)

/* ── The model ────────────────────────────────────────────────────── */

static void
board_window_free(gpointer data)
{
    ClawtOfficeBoardWindow *window = data;

    g_free(window->label);
    g_free(window->value);
    g_free(window->reset);
    g_free(window);
}

static void
board_login_free(gpointer data)
{
    ClawtOfficeBoardLogin *login = data;

    g_free(login->title);
    g_free(login->initial);
    g_free(login->status);
    g_ptr_array_unref(login->windows);
    g_free(login);
}

/* The first character of @title, upper case: the provider's mark. */
static gchar *
initial_of(const gchar *title)
{
    gunichar c;
    gchar out[8] = { 0 };

    if (title == NULL || *title == '\0' || !g_utf8_validate(title, -1, NULL))
        return g_strdup("?");
    c = g_unichar_toupper(g_utf8_get_char(title));
    g_unichar_to_utf8(c, out);
    return g_strdup(out);
}

/* A string member, or @fallback when it is absent or not a string. */
static const gchar *
text_member(JsonObject *object, const gchar *name, const gchar *fallback)
{
    JsonNode *node;

    if (object == NULL || !json_object_has_member(object, name))
        return fallback;
    node = json_object_get_member(object, name);
    if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING)
        return fallback;
    return json_node_get_string(node);
}

/* One window, worded by the calls the Fleet page makes. */
static ClawtOfficeBoardWindow *
board_window_new(JsonObject *window, gint64 now)
{
    ClawtOfficeBoardWindow *out = g_new0(ClawtOfficeBoardWindow, 1);

    out->label = g_strdup(text_member(window, "label", "Allowance"));
    out->value = clawt_usage_window_value_label(window);
    out->reset = clawt_usage_window_reset_label(window, now);
    out->tone = clawt_allowance_level_tone(clawt_usage_window_level(window));
    out->balance = g_strcmp0(text_member(window, "kind", "meter"), "balance") == 0;
    /* a balance has a figure, not a share: it is never a meter */
    out->has_left = !out->balance && clawt_usage_window_remaining(window, &out->left);
    if (out->has_left)
        out->left = CLAMP(out->left, 0.0, 100.0);
    return out;
}

ClawtOfficeBoard *
clawt_office_board_new(JsonObject *usage, gint64 now)
{
    ClawtOfficeBoard *self = g_new0(ClawtOfficeBoard, 1);
    JsonArray *logins = NULL;
    guint i;

    self->ref_count = 1;
    self->logins = g_ptr_array_new_with_free_func(board_login_free);
    if (usage != NULL && json_object_has_member(usage, "logins") &&
        JSON_NODE_HOLDS_ARRAY(json_object_get_member(usage, "logins")))
        logins = json_object_get_array_member(usage, "logins");

    for (i = 0; logins != NULL && i < json_array_get_length(logins); i++) {
        JsonNode *node = json_array_get_element(logins, i);
        JsonObject *login;
        JsonObject *tightest;
        JsonArray *windows = NULL;
        ClawtOfficeBoardLogin *out;
        guint w;

        if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node))
            continue;
        login = json_node_get_object(node);
        out = g_new0(ClawtOfficeBoardLogin, 1);
        out->title = clawt_usage_login_title(login);
        out->initial = initial_of(out->title);
        out->status = clawt_usage_login_status_label(login, now);
        out->tone = clawt_allowance_level_tone(clawt_usage_login_level(login));
        out->tightest = -1;
        out->windows = g_ptr_array_new_with_free_func(board_window_free);

        if (json_object_has_member(login, "windows") &&
            JSON_NODE_HOLDS_ARRAY(json_object_get_member(login, "windows")))
            windows = json_object_get_array_member(login, "windows");
        tightest = clawt_usage_login_tightest_window(login);

        /* every window: nothing is dropped here, a layout says what did not fit */
        for (w = 0; windows != NULL && w < json_array_get_length(windows); w++) {
            JsonNode *window_node = json_array_get_element(windows, w);
            JsonObject *window;

            if (window_node == NULL || !JSON_NODE_HOLDS_OBJECT(window_node))
                continue;
            window = json_node_get_object(window_node);
            if (window == tightest)
                out->tightest = (gint)out->windows->len;
            g_ptr_array_add(out->windows, board_window_new(window, now));
        }
        g_ptr_array_add(self->logins, out);
    }
    return self;
}

ClawtOfficeBoard *
clawt_office_board_ref(ClawtOfficeBoard *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    g_atomic_int_inc(&self->ref_count);
    return self;
}

void
clawt_office_board_free(ClawtOfficeBoard *self)
{
    if (self == NULL || !g_atomic_int_dec_and_test(&self->ref_count))
        return;
    g_ptr_array_unref(self->logins);
    g_free(self);
}

guint
clawt_office_board_n_windows(ClawtOfficeBoard *self)
{
    guint n = 0;
    guint i;

    g_return_val_if_fail(self != NULL, 0);

    for (i = 0; i < self->logins->len; i++)
        n += ((ClawtOfficeBoardLogin *)g_ptr_array_index(self->logins, i))->windows->len;
    return n;
}

JsonObject *
clawt_office_board_to_json(ClawtOfficeBoard *self)
{
    JsonObject *board = json_object_new();
    JsonArray *logins = json_array_new();
    guint i;
    guint w;

    g_return_val_if_fail(self != NULL, board);

    for (i = 0; i < self->logins->len; i++) {
        ClawtOfficeBoardLogin *login = g_ptr_array_index(self->logins, i);
        JsonObject *entry = json_object_new();
        JsonArray *windows = json_array_new();

        json_object_set_string_member(entry, "title", login->title);
        json_object_set_string_member(entry, "initial", login->initial);
        json_object_set_string_member(entry, "status", login->status);
        json_object_set_string_member(entry, "tone", login->tone);
        json_object_set_int_member(entry, "tightest", login->tightest);
        for (w = 0; w < login->windows->len; w++) {
            ClawtOfficeBoardWindow *window = g_ptr_array_index(login->windows, w);
            JsonObject *meter = json_object_new();

            json_object_set_string_member(meter, "label", window->label);
            json_object_set_string_member(meter, "value", window->value);
            if (window->reset != NULL)
                json_object_set_string_member(meter, "reset", window->reset);
            json_object_set_string_member(meter, "tone", window->tone);
            json_object_set_string_member(meter, "kind", window->balance ? "balance" : "meter");
            if (window->has_left)
                json_object_set_double_member(meter, "remaining", window->left);
            json_array_add_object_element(windows, meter);
        }
        json_object_set_array_member(entry, "windows", windows);
        json_array_add_object_element(logins, entry);
    }
    json_object_set_array_member(board, "logins", logins);
    return board;
}

/* ── Measuring ────────────────────────────────────────────────────── */

static gfloat
measure_text(ClawtOfficeBoardMeasure measure, gpointer user_data, const gchar *text,
             gfloat size)
{
    if (text == NULL || *text == '\0')
        return 0.0f;
    return measure(text, size, user_data);
}

gchar *
clawt_office_board_ellipsize(const gchar *text, gfloat room, gfloat size,
                             ClawtOfficeBoardMeasure measure, gpointer user_data)
{
    glong n;
    glong low;
    glong high;
    gchar *best = NULL;

    g_return_val_if_fail(measure != NULL, g_strdup(""));

    if (text == NULL)
        return g_strdup("");
    if (measure_text(measure, user_data, text, size) <= room)
        return g_strdup(text);
    if (measure(ELLIPSIS, size, user_data) > room)
        return g_strdup("");

    /*
     * The longest prefix, in whole characters, that fits with the
     * ellipsis after it.  Widths grow with length in any font this is
     * given, so a binary search finds it in a handful of measurements --
     * this runs for every text on every frame.
     */
    n = g_utf8_strlen(text, -1);
    low = 0;
    high = n - 1;
    best = g_strdup(ELLIPSIS);
    while (low <= high) {
        glong mid = (low + high) / 2;
        const gchar *end = g_utf8_offset_to_pointer(text, mid);
        g_autofree gchar *prefix = g_strndup(text, (gsize)(end - text));
        gchar *candidate;

        g_strchomp(prefix);
        candidate = g_strconcat(prefix, ELLIPSIS, NULL);
        if (measure(candidate, size, user_data) <= room) {
            g_free(best);
            best = candidate;
            low = mid + 1;
        } else {
            g_free(candidate);
            high = mid - 1;
        }
    }
    return best;
}

/* ── Items ────────────────────────────────────────────────────────── */

static void
item_clear(gpointer data)
{
    g_free(((ClawtOfficeBoardItem *)data)->text);
}

static ClawtOfficeBoardLayout *
layout_new(ClawtOfficeBoardDetail detail)
{
    ClawtOfficeBoardLayout *self = g_new0(ClawtOfficeBoardLayout, 1);

    self->ref_count = 1;
    self->detail = detail;
    self->items = g_array_new(FALSE, TRUE, sizeof(ClawtOfficeBoardItem));
    g_array_set_clear_func(self->items, item_clear);
    return self;
}

ClawtOfficeBoardLayout *
clawt_office_board_layout_ref(ClawtOfficeBoardLayout *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    g_atomic_int_inc(&self->ref_count);
    return self;
}

void
clawt_office_board_layout_free(ClawtOfficeBoardLayout *self)
{
    if (self == NULL || !g_atomic_int_dec_and_test(&self->ref_count))
        return;
    g_array_unref(self->items);
    g_free(self);
}

static void
add_box(ClawtOfficeBoardLayout *self, ClawtOfficeBoardItemKind kind, gfloat x, gfloat y,
        gfloat width, gfloat height, const gchar *tone)
{
    ClawtOfficeBoardItem item;

    memset(&item, 0, sizeof(item));
    item.kind = kind;
    item.x = x;
    item.y = y;
    item.width = MAX(width, 0.0f);
    item.height = MAX(height, 0.0f);
    item.tone = tone;
    g_array_append_val(self->items, item);
}

/* @text, ellipsized to @room, top-left at @x, @y. */
static void
add_text(ClawtOfficeBoardLayout *self, const gchar *text, gfloat x, gfloat y, gfloat room,
         gfloat size, ClawtOfficeBoardInk ink, ClawtOfficeBoardMeasure measure,
         gpointer user_data)
{
    ClawtOfficeBoardItem item;
    g_autofree gchar *fitted = clawt_office_board_ellipsize(text, room, size, measure,
                                                            user_data);

    if (*fitted == '\0')
        return;
    memset(&item, 0, sizeof(item));
    item.kind = CLAWT_OFFICE_BOARD_ITEM_TEXT;
    item.x = x;
    item.y = y;
    item.width = measure_text(measure, user_data, fitted, size);
    item.height = size;
    item.size = size;
    item.room = room;
    item.text = g_steal_pointer(&fitted);
    item.ink = ink;
    g_array_append_val(self->items, item);
}

/* A meter: its track, and what is left over it in @tone. */
static void
add_meter(ClawtOfficeBoardLayout *self, gfloat x, gfloat y, gfloat width, gfloat height,
          gboolean has_left, gdouble left, const gchar *tone)
{
    add_box(self, CLAWT_OFFICE_BOARD_ITEM_TRACK, x, y, width, height, NULL);
    if (has_left && left > 0.0)
        add_box(self, CLAWT_OFFICE_BOARD_ITEM_FILL, x, y,
                width * (gfloat)(CLAMP(left, 0.0, 100.0) / 100.0), height, tone);
}

static gint
tone_rank(const gchar *tone)
{
    if (g_strcmp0(tone, "bad") == 0)
        return 3;
    if (g_strcmp0(tone, "warn") == 0)
        return 2;
    if (g_strcmp0(tone, "good") == 0)
        return 1;
    return 0;
}

/* ── On the wall ──────────────────────────────────────────────────── */

/*
 * Rows: per login its title, then per window its label, meter and
 * figure (or its status, when it has none).  NULL when it does not fit
 * the zone at @size.
 */
static ClawtOfficeBoardLayout *
world_rows(ClawtOfficeBoard *board, gfloat zx, gfloat zy, gfloat zw, gfloat zh, gfloat ui,
           gfloat size, ClawtOfficeBoardMeasure measure, gpointer user_data)
{
    ClawtOfficeBoardLayout *self;
    gfloat pad = MAX(2.0f * ui, floorf(size * 0.4f));
    gfloat line = floorf(size * 1.4f);
    gfloat gap = MAX(2.0f * ui, floorf(size * 0.4f));
    gfloat inner = zw - 2.0f * pad;
    gfloat value_w = 0.0f;
    gfloat bar_w;
    gfloat label_w;
    gfloat y;
    guint lines = 0;
    guint i;
    guint w;

    for (i = 0; i < board->logins->len; i++) {
        ClawtOfficeBoardLogin *login = g_ptr_array_index(board->logins, i);

        lines += 1 + MAX(login->windows->len, 1);
        for (w = 0; w < login->windows->len; w++) {
            ClawtOfficeBoardWindow *window = g_ptr_array_index(login->windows, w);

            value_w = MAX(value_w, measure_text(measure, user_data, window->value, size));
        }
    }
    if (pad * 2.0f + (gfloat)lines * line > zh)
        return NULL;

    /* the figure gets what it needs up to 40%; the meter a fifth; the name the rest */
    value_w = MIN(ceilf(value_w), floorf(inner * 0.4f));
    bar_w = floorf(MAX(inner * 0.2f, size));
    label_w = inner - value_w - bar_w - 2.0f * gap;
    if (label_w < size * 2.5f)
        return NULL;

    self = layout_new(CLAWT_OFFICE_BOARD_DETAIL_ROWS);
    self->x = zx;
    self->y = zy;
    self->width = zw;
    self->height = zh;
    add_box(self, CLAWT_OFFICE_BOARD_ITEM_BACKING, zx, zy, zw, zh, NULL);

    y = zy + pad;
    for (i = 0; i < board->logins->len; i++) {
        ClawtOfficeBoardLogin *login = g_ptr_array_index(board->logins, i);
        gfloat text_dy = floorf((line - size) / 2.0f);

        add_text(self, login->title, zx + pad, y + text_dy, inner, size,
                 CLAWT_OFFICE_BOARD_INK_TITLE, measure, user_data);
        y += line;
        if (login->windows->len == 0) {
            add_text(self, login->status, zx + pad, y + text_dy, inner, size,
                     CLAWT_OFFICE_BOARD_INK_MUTED, measure, user_data);
            y += line;
            continue;
        }
        for (w = 0; w < login->windows->len; w++) {
            ClawtOfficeBoardWindow *window = g_ptr_array_index(login->windows, w);
            gfloat bar_x = zx + pad + label_w + gap;
            gfloat bar_h = MAX(2.0f * ui, floorf(size * 0.6f));

            add_text(self, window->label, zx + pad, y + text_dy, label_w, size,
                     CLAWT_OFFICE_BOARD_INK_LABEL, measure, user_data);
            if (!window->balance)
                add_meter(self, bar_x, y + floorf((line - bar_h) / 2.0f), bar_w, bar_h,
                          window->has_left, window->left, window->tone);
            add_text(self, window->value, bar_x + bar_w + gap, y + text_dy, value_w, size,
                     CLAWT_OFFICE_BOARD_INK_VALUE, measure, user_data);
            y += line;
        }
    }
    return self;
}

/*
 * The side of the badge in a grid of @cells gauges with @cols columns,
 * and the cell size.  A gauge is a square badge and a meter beside it
 * at least as long, so a cell is at least twice as wide as it is tall.
 */
static gfloat
gauge_side(guint cells, guint cols, gfloat zw, gfloat zh, gfloat pad, gfloat cap,
           gfloat *cell_w, gfloat *cell_h)
{
    guint rows = (cells + cols - 1) / cols;

    *cell_w = (zw - pad * (gfloat)(cols + 1)) / (gfloat)cols;
    *cell_h = (zh - pad * (gfloat)(rows + 1)) / (gfloat)rows;
    return floorf(MIN(MIN(*cell_h, *cell_w / 2.2f), cap));
}

/* The best grid for @cells gauges: the column count giving the largest badge. */
static gfloat
best_grid(guint cells, gfloat zw, gfloat zh, gfloat pad, gfloat cap, guint *out_cols,
          gfloat *out_w, gfloat *out_h)
{
    gfloat best = -1.0f;
    guint cols;

    for (cols = 1; cols <= cells; cols++) {
        gfloat cell_w;
        gfloat cell_h;
        gfloat side = gauge_side(cells, cols, zw, zh, pad, cap, &cell_w, &cell_h);

        if (side > best) {
            best = side;
            *out_cols = cols;
            *out_w = cell_w;
            *out_h = cell_h;
        }
    }
    return best;
}

/*
 * Gauges: per login its initial on a badge and a meter for its tightest
 * window, in a grid filling the zone.  When they do not all fit at a
 * legible size the last cell says "+N".  NULL when not even one does.
 */
static ClawtOfficeBoardLayout *
world_gauges(ClawtOfficeBoard *board, gfloat zx, gfloat zy, gfloat zw, gfloat zh, gfloat ui,
             ClawtOfficeBoardMeasure measure, gpointer user_data)
{
    ClawtOfficeBoardLayout *self;
    gfloat text = CLAWT_OFFICE_BOARD_MIN_TEXT * ui;
    gfloat least = text + 4.0f * ui;           /* a badge the letter fits on */
    gfloat cap = 40.0f * ui;                   /* and no larger: it is a mark */
    gfloat pad = MAX(2.0f * ui, floorf(MIN(zw, zh) * 0.05f));
    guint n = board->logins->len;
    guint shown = n;
    guint cells = n;
    guint cols = 1;
    gfloat cell_w = 0.0f;
    gfloat cell_h = 0.0f;
    gfloat side = -1.0f;
    guint i;

    /* all of them; else all but k and a "+k" cell, fewest dropped first */
    for (shown = n; ; shown--) {
        cells = shown == n ? n : shown + 1;
        side = best_grid(cells, zw, zh, pad, cap, &cols, &cell_w, &cell_h);
        if (side >= least || shown == 0)
            break;
    }
    if (side < least || shown == 0)
        return NULL;

    self = layout_new(CLAWT_OFFICE_BOARD_DETAIL_GAUGES);
    self->x = zx;
    self->y = zy;
    self->width = zw;
    self->height = zh;
    self->hidden = n - shown;
    add_box(self, CLAWT_OFFICE_BOARD_ITEM_BACKING, zx, zy, zw, zh, NULL);

    for (i = 0; i < cells; i++) {
        gfloat cx = zx + pad + (gfloat)(i % cols) * (cell_w + pad);
        gfloat cy = zy + pad + (gfloat)(i / cols) * (cell_h + pad);
        gfloat by = floorf(cy + (cell_h - side) / 2.0f);

        if (i == shown) {
            /* the last cell counts the rest */
            g_autofree gchar *more = g_strdup_printf("+%u", n - shown);
            gfloat width = measure_text(measure, user_data, more, text);

            add_text(self, more, cx + MAX(0.0f, floorf((cell_w - width) / 2.0f)),
                     floorf(cy + (cell_h - text) / 2.0f), cell_w, text,
                     CLAWT_OFFICE_BOARD_INK_MUTED, measure, user_data);
            continue;
        }
        {
            ClawtOfficeBoardLogin *login = g_ptr_array_index(board->logins, i);
            ClawtOfficeBoardWindow *tight = login->tightest >= 0
                ? g_ptr_array_index(login->windows, login->tightest) : NULL;
            gfloat letter = side >= 2.0f * text + 4.0f * ui ? 2.0f * text : text;
            gfloat width = measure_text(measure, user_data, login->initial, letter);
            gfloat gap = MAX(2.0f * ui, floorf(side * 0.25f));
            gfloat bar_h = MAX(3.0f * ui, floorf(side * 0.4f));
            gfloat bar_x = cx + side + gap;

            add_box(self, CLAWT_OFFICE_BOARD_ITEM_BADGE, cx, by, side, side,
                    tight != NULL ? tight->tone : login->tone);
            add_text(self, login->initial, cx + MAX(0.0f, floorf((side - width) / 2.0f)),
                     by + floorf((side - letter) / 2.0f), side, letter,
                     CLAWT_OFFICE_BOARD_INK_ON_BADGE, measure, user_data);
            add_meter(self, bar_x, by + floorf((side - bar_h) / 2.0f),
                      cx + cell_w - bar_x, bar_h, tight != NULL && tight->has_left,
                      tight != NULL ? tight->left : 0.0, tight != NULL ? tight->tone : NULL);
        }
    }
    return self;
}

/* One badge in the worst tone, as large as the zone allows. */
static ClawtOfficeBoardLayout *
world_mark(ClawtOfficeBoard *board, gfloat zx, gfloat zy, gfloat zw, gfloat zh)
{
    ClawtOfficeBoardLayout *self;
    const gchar *worst = "neutral";
    gfloat side = floorf(MIN(zw, zh));
    guint i;

    if (side < 2.0f)
        return layout_new(CLAWT_OFFICE_BOARD_DETAIL_NONE);
    for (i = 0; i < board->logins->len; i++) {
        ClawtOfficeBoardLogin *login = g_ptr_array_index(board->logins, i);

        if (tone_rank(login->tone) > tone_rank(worst))
            worst = login->tone;
    }
    self = layout_new(CLAWT_OFFICE_BOARD_DETAIL_MARK);
    self->x = floorf(zx + (zw - side) / 2.0f);
    self->y = floorf(zy + (zh - side) / 2.0f);
    self->width = side;
    self->height = side;
    self->hidden = board->logins->len;
    add_box(self, CLAWT_OFFICE_BOARD_ITEM_BADGE, self->x, self->y, side, side, worst);
    return self;
}

ClawtOfficeBoardLayout *
clawt_office_board_layout_world(ClawtOfficeBoard *board, gfloat zone_x, gfloat zone_y,
                                gfloat zone_width, gfloat zone_height, gint ui_scale,
                                ClawtOfficeBoardMeasure measure, gpointer user_data)
{
    ClawtOfficeBoardLayout *self;
    gfloat ui = (gfloat)MAX(ui_scale, 1);
    gfloat zx = floorf(zone_x);
    gfloat zy = floorf(zone_y);
    gfloat zw = floorf(zone_width);
    gfloat zh = floorf(zone_height);

    g_return_val_if_fail(board != NULL, NULL);
    g_return_val_if_fail(measure != NULL, NULL);

    if (board->logins->len == 0 || zw < 1.0f || zh < 1.0f)
        return layout_new(CLAWT_OFFICE_BOARD_DETAIL_NONE);

    /*
     * The most that fits, largest text first: twice the pixel font's size
     * (crisp: whole multiples only), then its own size; then gauges;
     * then the mark.  Nothing here looks at the zoom -- the zone's size
     * on screen is the zoom, the view and the device scale at once.
     */
    self = world_rows(board, zx, zy, zw, zh, ui, 2.0f * CLAWT_OFFICE_BOARD_MIN_TEXT * ui,
                      measure, user_data);
    if (self == NULL)
        self = world_rows(board, zx, zy, zw, zh, ui, CLAWT_OFFICE_BOARD_MIN_TEXT * ui,
                          measure, user_data);
    if (self == NULL)
        self = world_gauges(board, zx, zy, zw, zh, ui, measure, user_data);
    if (self == NULL)
        self = world_mark(board, zx, zy, zw, zh);
    return self;
}

/* ── The details panel ────────────────────────────────────────────── */

typedef enum {
    ROW_HEADING,
    ROW_TITLE,
    ROW_STATUS,
    ROW_WINDOW,
    ROW_MORE
} RowKind;

typedef struct {
    RowKind                 kind;
    ClawtOfficeBoardLogin  *login;
    ClawtOfficeBoardWindow *window;
    gfloat                  height;
} Row;

/* Narrows *@width towards @floor by up to *@excess, and says how much is left over. */
static void
shrink(gfloat *width, gfloat floor_width, gfloat *excess)
{
    gfloat take;

    if (*excess <= 0.0f || *width <= floor_width)
        return;
    take = MIN(*excess, *width - floor_width);
    *width -= take;
    *excess -= take;
}

ClawtOfficeBoardLayout *
clawt_office_board_layout_panel(ClawtOfficeBoard *board, gboolean docked,
                                gfloat anchor_x, gfloat anchor_y, gfloat anchor_width,
                                gfloat anchor_height, gfloat view_width, gfloat view_height,
                                gint ui_scale, ClawtOfficeBoardMeasure measure,
                                gpointer user_data)
{
    static const gchar heading[] = "Provider usage";
    ClawtOfficeBoardLayout *self;
    gfloat ui = (gfloat)MAX(ui_scale, 1);
    gfloat size = CLAWT_OFFICE_BOARD_PANEL_TEXT * ui;
    gfloat line = floorf(size * 1.6f);
    gfloat pad = 8.0f * ui;
    gfloat gap = 8.0f * ui;
    gfloat margin = 8.0f * ui;
    gfloat bar_w = 60.0f * ui;
    gfloat bar_h = MAX(3.0f * ui, floorf(size * 0.6f));
    gfloat label_w = 0.0f;
    gfloat value_w = 0.0f;
    gfloat reset_w = 0.0f;
    gfloat title_w;
    gfloat row_w;
    gfloat content_w;
    gfloat max_content;
    gfloat max_height;
    gfloat used;
    gfloat px;
    gfloat py;
    gfloat y;
    gboolean resets = FALSE;
    g_autoptr(GArray) rows = g_array_new(FALSE, FALSE, sizeof(Row));
    guint n_rows;
    guint hidden = 0;
    guint i;
    guint w;

    g_return_val_if_fail(board != NULL, NULL);
    g_return_val_if_fail(measure != NULL, NULL);

    if (board->logins->len == 0)
        return layout_new(CLAWT_OFFICE_BOARD_DETAIL_NONE);

    /* ── every row, and every column's natural width ── */
    title_w = measure_text(measure, user_data, heading, size);
    {
        Row row = { ROW_HEADING, NULL, NULL, line };

        g_array_append_val(rows, row);
    }
    for (i = 0; i < board->logins->len; i++) {
        ClawtOfficeBoardLogin *login = g_ptr_array_index(board->logins, i);
        Row title = { ROW_TITLE, login, NULL, line + floorf(line * 0.4f) };
        Row status = { ROW_STATUS, login, NULL, line };

        g_array_append_val(rows, title);
        g_array_append_val(rows, status);
        title_w = MAX(title_w, measure_text(measure, user_data, login->title, size));
        title_w = MAX(title_w, measure_text(measure, user_data, login->status, size));
        for (w = 0; w < login->windows->len; w++) {
            ClawtOfficeBoardWindow *window = g_ptr_array_index(login->windows, w);
            Row row = { ROW_WINDOW, login, window, line };

            g_array_append_val(rows, row);
            label_w = MAX(label_w, measure_text(measure, user_data, window->label, size));
            value_w = MAX(value_w, measure_text(measure, user_data, window->value, size));
            if (window->reset != NULL) {
                resets = TRUE;
                reset_w = MAX(reset_w, measure_text(measure, user_data, window->reset, size));
            }
        }
    }
    label_w = MIN(ceilf(label_w), 200.0f * ui);
    value_w = MIN(ceilf(value_w), 220.0f * ui);
    reset_w = MIN(ceilf(reset_w), 160.0f * ui);
    title_w = MIN(ceilf(title_w), 360.0f * ui);

    /* ── wider than the view: the reset gives first, then the name, then the figure ── */
    max_content = MAX(view_width - 2.0f * margin - 2.0f * pad, 0.0f);
    row_w = label_w + gap + bar_w + gap + value_w + (resets ? gap + reset_w : 0.0f);
    if (row_w > max_content) {
        gfloat excess = row_w - max_content;

        shrink(&reset_w, 48.0f * ui, &excess);
        shrink(&label_w, 40.0f * ui, &excess);
        if (excess > 0.0f && resets) {
            excess -= gap + reset_w;
            resets = FALSE;
            reset_w = 0.0f;
            /* dropping the column may have freed more than was needed */
            if (excess < 0.0f) {
                label_w += MIN(-excess, 200.0f * ui - label_w);
                excess = 0.0f;
            }
        }
        shrink(&value_w, 48.0f * ui, &excess);
        shrink(&bar_w, 16.0f * ui, &excess);
        shrink(&label_w, 16.0f * ui, &excess);
        row_w = label_w + gap + bar_w + gap + value_w + (resets ? gap + reset_w : 0.0f);
    }
    title_w = MIN(title_w, max_content);
    content_w = MIN(MAX(row_w, title_w), max_content);
    title_w = content_w;

    /* ── taller than the view: as many rows as fit, then "+N more" ── */
    max_height = MAX(view_height - 2.0f * margin - 2.0f * pad, line);
    used = 0.0f;
    for (i = 0; i < rows->len; i++)
        used += g_array_index(rows, Row, i).height;
    n_rows = rows->len;
    if (used > max_height) {
        used = 0.0f;
        for (n_rows = 0; n_rows < rows->len; n_rows++) {
            gfloat next = g_array_index(rows, Row, n_rows).height;

            if (used + next + line > max_height)
                break;
            used += next;
        }
        /* a title or status with none of its windows under it says nothing */
        while (n_rows > 1 && g_array_index(rows, Row, n_rows - 1).kind != ROW_WINDOW &&
               g_array_index(rows, Row, n_rows - 1).kind != ROW_HEADING) {
            n_rows--;
            used -= g_array_index(rows, Row, n_rows).height;
        }
        for (i = n_rows; i < rows->len; i++) {
            Row *row = &g_array_index(rows, Row, i);

            /* what is not shown: each window, and each login that has none */
            if (row->kind == ROW_WINDOW ||
                (row->kind == ROW_TITLE && row->login->windows->len == 0))
                hidden++;
        }
        used += line;
    }

    self = layout_new(CLAWT_OFFICE_BOARD_DETAIL_PANEL);
    self->width = floorf(content_w + 2.0f * pad);
    self->height = floorf(used + 2.0f * pad);
    self->hidden = hidden;

    /* ── where: docked top right, or beside the board -- and inside the view ── */
    if (docked) {
        px = view_width - margin - self->width;
        py = margin;
    } else {
        px = anchor_x + anchor_width + gap;
        py = anchor_y;
        if (px + self->width > view_width - margin)
            px = anchor_x - gap - self->width;
        if (px < margin)
            px = anchor_x + (anchor_width - self->width) / 2.0f;
    }
    px = floorf(CLAMP(px, margin, MAX(margin, view_width - margin - self->width)));
    py = floorf(CLAMP(py, margin, MAX(margin, view_height - margin - self->height)));
    self->x = px;
    self->y = py;
    add_box(self, CLAWT_OFFICE_BOARD_ITEM_BACKING, px, py, self->width, self->height, NULL);

    /* ── the rows ── */
    y = py + pad;
    for (i = 0; i < n_rows; i++) {
        Row *row = &g_array_index(rows, Row, i);
        gfloat text_y = y + row->height - line + floorf((line - size) / 2.0f);
        gfloat x = px + pad;

        switch (row->kind) {
        case ROW_HEADING:
            add_text(self, heading, x, text_y, title_w, size, CLAWT_OFFICE_BOARD_INK_TITLE,
                     measure, user_data);
            break;
        case ROW_TITLE:
            add_text(self, row->login->title, x, text_y, title_w, size,
                     CLAWT_OFFICE_BOARD_INK_TITLE, measure, user_data);
            break;
        case ROW_STATUS:
            add_text(self, row->login->status, x, text_y, title_w, size,
                     CLAWT_OFFICE_BOARD_INK_MUTED, measure, user_data);
            break;
        case ROW_WINDOW: {
            ClawtOfficeBoardWindow *window = row->window;
            gfloat bar_x = x + label_w + gap;
            gfloat value_x = bar_x + bar_w + gap;

            add_text(self, window->label, x, text_y, label_w, size,
                     CLAWT_OFFICE_BOARD_INK_LABEL, measure, user_data);
            /* a balance is a figure: no meter to read as "used up" */
            if (!window->balance)
                add_meter(self, bar_x, y + floorf((line - bar_h) / 2.0f), bar_w, bar_h,
                          window->has_left, window->left, window->tone);
            add_text(self, window->value, value_x, text_y, value_w, size,
                     CLAWT_OFFICE_BOARD_INK_VALUE, measure, user_data);
            if (resets && window->reset != NULL)
                add_text(self, window->reset, value_x + value_w + gap, text_y, reset_w, size,
                         CLAWT_OFFICE_BOARD_INK_MUTED, measure, user_data);
            break;
        }
        case ROW_MORE:
        default:
            break;
        }
        y += row->height;
    }
    if (n_rows < rows->len) {
        g_autofree gchar *more = g_strdup_printf("+%u more", hidden);

        add_text(self, more, px + pad, y + floorf((line - size) / 2.0f), title_w, size,
                 CLAWT_OFFICE_BOARD_INK_MUTED, measure, user_data);
    }
    return self;
}
