/*
 * lrg-board-model.c - Rows the agent board draws
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * See clawt-lrg-board-model.h.  No libregnum and no GTK: the GTK client
 * compiles this file too, and tests/test-lrg-models-board.c includes it.
 */

#include "clawt-lrg-board-model.h"

#include <string.h>

/* A note longer than this is cut, with an ellipsis. */
#define NOTE_CHARS (96)

/* A title longer than this is cut the same way. */
#define TITLE_CHARS (80)

static const gchar *view_labels[] = {
    "List",
    "Board",
    "Timeline"
};

/* The seven columns, in the order the reply always uses. */
static const gchar *column_ids[] = {
    "blocked",
    "backlog",
    "planning",
    "delegated",
    "review",
    "gating",
    "done"
};

/* Events that mean the reply is stale.  turn.step is patched in place. */
static const gchar *refresh_kinds[] = {
    "task.changed",
    "task.noted",
    "agent.state",
    "mailbox.queued",
    "mailbox.delivered",
    "decision.asked",
    "decision.settled",
    "decision.deleted",
    "wait.changed",
    NULL
};

/* ── Small readers ────────────────────────────────────────────────── */

static JsonObject *
obj_member(JsonObject *obj, const gchar *key)
{
    JsonNode *node;

    if (obj == NULL || !json_object_has_member(obj, key))
        return NULL;

    node = json_object_get_member(obj, key);

    if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node))
        return NULL;

    return json_node_get_object(node);
}

static JsonArray *
array_member(JsonObject *obj, const gchar *key)
{
    JsonNode *node;

    if (obj == NULL || !json_object_has_member(obj, key))
        return NULL;

    node = json_object_get_member(obj, key);

    if (node == NULL || !JSON_NODE_HOLDS_ARRAY(node))
        return NULL;

    return json_node_get_array(node);
}

static gboolean
has_double(JsonObject *obj, const gchar *key)
{
    JsonNode *node;

    if (obj == NULL || !json_object_has_member(obj, key))
        return FALSE;

    node = json_object_get_member(obj, key);

    return node != NULL && JSON_NODE_HOLDS_VALUE(node) &&
           json_node_get_value_type(node) == G_TYPE_DOUBLE;
}

/* ── Views and labels ─────────────────────────────────────────────── */

guint
clawt_lrg_board_view_count(void)
{
    return G_N_ELEMENTS(view_labels);
}

const gchar *
clawt_lrg_board_view_label(ClawtLrgBoardView view)
{
    if ((guint)view >= G_N_ELEMENTS(view_labels))
        return view_labels[0];

    return view_labels[view];
}

gboolean
clawt_lrg_board_column_starts_collapsed(const gchar *id)
{
    return g_strcmp0(id, "done") == 0;
}

const gchar *
clawt_lrg_board_state_tone(const gchar *state)
{
    gint value = 0;

    if (state != NULL &&
        clawt_enum_from_nick(CLAWT_TYPE_TASK_STATE, state, &value))
        return clawt_task_state_tone((ClawtTaskState)value);

    return "neutral";
}

gchar *
clawt_lrg_board_initials(const gchar *name)
{
    gunichar letters[2];
    guint n = 0;
    const gchar *p;
    gboolean word = TRUE;

    if (name == NULL || *name == '\0')
        return g_strdup("?");

    for (p = name; *p != '\0' && n < 2; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);

        if (g_unichar_isspace(c)) {
            word = TRUE;
            continue;
        }

        if (!word || !g_unichar_isalnum(c))
            continue;

        letters[n++] = g_unichar_toupper(c);
        word = FALSE;
    }

    if (n == 0)
        return g_strdup("?");

    return g_ucs4_to_utf8(letters, (glong)n, NULL, NULL, NULL);
}

gchar *
clawt_lrg_board_one_line(const gchar *text, guint max_chars)
{
    GString *out;
    const gchar *p;
    gboolean gap = FALSE;
    guint chars = 0;
    gboolean cut = FALSE;

    if (text == NULL)
        return g_strdup("");

    out = g_string_new(NULL);

    for (p = text; *p != '\0'; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);

        if (g_unichar_isspace(c)) {
            gap = out->len > 0;
            continue;
        }

        /*
         * Spaces count.  A limit that ignored them would keep a
         * sentence long past the line a card has room for.
         */
        if (gap && chars + 1 >= max_chars) {
            cut = TRUE;
            break;
        }

        if (chars >= max_chars) {
            cut = TRUE;
            break;
        }

        if (gap) {
            g_string_append_c(out, ' ');
            gap = FALSE;
            chars++;
        }

        g_string_append_unichar(out, c);
        chars++;
    }

    if (cut)
        g_string_append(out, "\xe2\x80\xa6");

    return g_string_free(out, FALSE);
}

gchar *
clawt_lrg_board_wait_label(gint64 seconds)
{
    return clawt_board_duration_label(seconds);
}

gchar *
clawt_lrg_board_chip_text(const gchar *initials,
                          const gchar *state,
                          const gchar *elapsed,
                          const gchar *after_label)
{
    const gchar *who = (initials != NULL && *initials != '\0') ? initials : "?";
    const gchar *st = (state != NULL && *state != '\0') ? state : "?";
    const gchar *el = (elapsed != NULL) ? elapsed : "0s";

    if (after_label != NULL && *after_label != '\0')
        return g_strdup_printf("%s %s %s, %s", who, st, el, after_label);

    return g_strdup_printf("%s %s %s", who, st, el);
}

gchar *
clawt_lrg_board_ticket_url(JsonObject *ticket)
{
    const gchar *base;
    const gchar *url;
    gint64 id;
    gint64 org;

    if (ticket == NULL)
        return NULL;

    id = clawt_ipc_payload_int(ticket, "id", 0);
    base = clawt_ipc_payload_string(ticket, "base");
    org = clawt_ipc_payload_int(ticket, "organization_id", 0);

    if (base != NULL && *base != '\0' && id > 0)
        return clawt_venture_record_url(base, "ticket", id, org);

    url = clawt_ipc_payload_string(ticket, "url");

    if (url == NULL || *url == '\0')
        return NULL;

    return g_strdup(url);
}

void
clawt_lrg_board_axis(gint64 since, gint64 now, const gchar *spec,
                     gint64 *from, gint64 *to, gdouble *now_x)
{
    gint64 axis_from;
    gint64 axis_to;
    gboolean today;

    axis_from = since;

    if (axis_from <= 0)
        axis_from = now;

    today = (spec == NULL || *spec == '\0' || g_strcmp0(spec, "today") == 0);

    if (today && now < axis_from + (gint64)86400)
        axis_to = axis_from + (gint64)86400;
    else if (now > axis_from)
        axis_to = now;
    else
        axis_to = axis_from + 1;

    if (from != NULL)
        *from = axis_from;

    if (to != NULL)
        *to = axis_to;

    if (now_x == NULL)
        return;

    if (now <= axis_from)
        *now_x = 0.0;
    else if (now >= axis_to)
        *now_x = 1.0;
    else
        *now_x = (gdouble)(now - axis_from) / (gdouble)(axis_to - axis_from);
}

gboolean
clawt_lrg_board_span(gint64 axis_from, gint64 axis_to,
                     gint64 bar_from, gint64 bar_to,
                     gdouble *x0, gdouble *x1)
{
    gint64 start;
    gint64 end;
    gint64 width;

    if (x0 != NULL)
        *x0 = 0.0;

    if (x1 != NULL)
        *x1 = 0.0;

    if (x0 == NULL || x1 == NULL)
        return FALSE;

    if (axis_to <= axis_from)
        return FALSE;

    if (bar_to <= bar_from)
        bar_to = bar_from;

    if (bar_to <= axis_from || bar_from >= axis_to)
        return FALSE;

    start = MAX(bar_from, axis_from);
    end = MIN(bar_to, axis_to);
    width = axis_to - axis_from;
    *x0 = (gdouble)(start - axis_from) / (gdouble)width;
    *x1 = (gdouble)(end - axis_from) / (gdouble)width;

    if (*x1 < *x0)
        *x1 = *x0;

    return TRUE;
}

gchar *
clawt_lrg_board_bar_cells(gdouble x0, gdouble x1, guint cells)
{
    GString *out;
    guint i;
    guint start;
    guint end;

    if (cells == 0)
        return g_strdup("");

    if (x0 < 0.0)
        x0 = 0.0;

    if (x1 > 1.0)
        x1 = 1.0;

    if (x1 < x0)
        x1 = x0;

    start = (guint)(x0 * (gdouble)cells);
    end = (guint)(x1 * (gdouble)cells + 0.999);

    if (end <= start && x1 > x0)
        end = start + 1;

    if (start >= cells)
        start = cells - 1;

    if (end > cells)
        end = cells;

    out = g_string_sized_new(cells * 3);

    for (i = 0; i < cells; i++)
        g_string_append(out, (i >= start && i < end)
                                 ? "\xe2\x96\x88" : "\xe2\x96\x91");

    return g_string_free(out, FALSE);
}

gboolean
clawt_lrg_board_event_refreshes(const gchar *kind)
{
    guint i;

    if (kind == NULL)
        return FALSE;

    for (i = 0; refresh_kinds[i] != NULL; i++) {
        if (g_strcmp0(kind, refresh_kinds[i]) == 0)
            return TRUE;
    }

    return FALSE;
}

gboolean
clawt_lrg_board_event_is_step(const gchar *kind)
{
    return g_strcmp0(kind, "turn.step") == 0;
}

gchar *
clawt_lrg_board_step_line(const gchar *kind_nick,
                          const gchar *agent,
                          const gchar *tool,
                          const gchar *text,
                          const gchar *detail,
                          gboolean     failed)
{
    gint value = CLAWT_STEP_TOOL;
    g_autoptr(ClawtTurnStep) step = NULL;

    if (kind_nick != NULL)
        clawt_enum_from_nick(CLAWT_TYPE_STEP_KIND, kind_nick, &value);

    step = clawt_turn_step_new((ClawtStepKind)value, agent, NULL,
                               text, tool, detail, failed);

    if (step == NULL)
        return g_strdup("");

    return clawt_turn_step_summary(step);
}

/* ── Freeing ──────────────────────────────────────────────────────── */

static void
chip_free(gpointer data)
{
    ClawtLrgBoardChip *chip = data;

    if (chip == NULL)
        return;

    g_free(chip->worker);
    g_free(chip->title);
    g_free(chip->initials);
    g_free(chip->state);
    g_free(chip->elapsed_label);
    g_free(chip->after_label);
    g_free(chip->text);
    g_free(chip);
}

static void
card_free(gpointer data)
{
    ClawtLrgBoardCard *card = data;

    if (card == NULL)
        return;

    g_free(card->id);
    g_free(card->title);
    g_free(card->state);
    g_free(card->column);
    g_free(card->repo);
    g_free(card->note);
    g_free(card->elapsed_label);
    g_free(card->cost_label);
    g_free(card->ticket_label);
    g_free(card->ticket_url);
    g_free(card->review_wait_label);
    g_free(card->review_for);
    g_free(card->blocks_line);
    g_clear_pointer(&card->chips, g_ptr_array_unref);
    g_free(card);
}

static void
column_free(gpointer data)
{
    ClawtLrgBoardColumn *column = data;

    if (column == NULL)
        return;

    g_free(column->id);
    g_free(column->label);
    g_clear_pointer(&column->cards, g_ptr_array_unref);
    g_free(column);
}

static void
lane_free(gpointer data)
{
    ClawtLrgBoardLane *lane = data;

    if (lane == NULL)
        return;

    g_free(lane->agent);
    g_free(lane->name);
    g_free(lane->state);
    g_free(lane->current_id);
    g_free(lane->current_title);
    g_free(lane->step_text);
    g_free(lane->queue_label);
    g_free(lane->activity_label);
    g_free(lane);
}

static void
bar_free(gpointer data)
{
    ClawtLrgBoardBar *bar = data;

    if (bar == NULL)
        return;

    g_free(bar->task_id);
    g_free(bar->label);
    g_free(bar->state);
    g_free(bar->tone);
    g_free(bar);
}

static void
header_clear(ClawtLrgBoardHeader *header)
{
    g_free(header->agent);
    g_free(header->name);
    g_free(header->state);
    g_free(header->team);
    g_free(header->step_text);
    g_free(header->current_id);
    g_free(header->current_title);
    g_free(header->activity_label);
    g_free(header->counts_line);
    memset(header, 0, sizeof(*header));
}

void
clawt_lrg_board_free(ClawtLrgBoard *board)
{
    if (board == NULL)
        return;

    header_clear(&board->header);
    g_clear_pointer(&board->columns, g_ptr_array_unref);
    g_clear_pointer(&board->lanes, g_ptr_array_unref);
    g_clear_pointer(&board->bars, g_ptr_array_unref);
    g_free(board->window_spec);
    g_free(board->agent);
    g_free(board->team);
    g_free(board);
}

/* ── Parsing ──────────────────────────────────────────────────────── */

static gchar *
activity_label(JsonObject *who, gint64 now)
{
    gint64 busy;
    gint64 idle;

    if (who == NULL)
        return NULL;

    busy = clawt_ipc_payload_int(who, "busy_since", 0);

    if (busy > 0)
        return g_strdup_printf("busy %s",
                               clawt_board_duration_label(
                                   MAX(now - busy, 0)));

    if (!json_object_has_member(who, "idle"))
        return NULL;

    idle = clawt_ipc_payload_int(who, "idle", 0);

    return g_strdup_printf("idle %s", clawt_board_duration_label(idle));
}

/*
 * "waits on X" for one chip.  @after names sibling task ids; a sibling
 * whose title we already parsed is named by that title, and an id with
 * no sibling chip stays an id.
 */
static gchar *
chip_after_label(GPtrArray *chips, JsonArray *raw_chips, JsonArray *after)
{
    GString *out;
    guint i;

    if (after == NULL || json_array_get_length(after) == 0)
        return NULL;

    out = g_string_new("waits on ");

    for (i = 0; i < json_array_get_length(after); i++) {
        JsonNode *node = json_array_get_element(after, i);
        const gchar *id;
        const gchar *shown = NULL;
        guint c;

        if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) ||
            json_node_get_value_type(node) != G_TYPE_STRING)
            continue;

        id = json_node_get_string(node);

        for (c = 0; raw_chips != NULL && c < chips->len &&
             c < json_array_get_length(raw_chips); c++) {
            JsonObject *sib = json_array_get_object_element(raw_chips, c);
            ClawtLrgBoardChip *other = g_ptr_array_index(chips, c);

            if (g_strcmp0(clawt_ipc_payload_string(sib, "id"), id) != 0)
                continue;

            if (other->title != NULL && *other->title != '\0')
                shown = other->title;

            break;
        }

        if (out->len > (guint)strlen("waits on "))
            g_string_append(out, ", ");

        g_string_append(out, (shown != NULL) ? shown : id);
    }

    if (out->len == (guint)strlen("waits on ")) {
        g_string_free(out, TRUE);
        return NULL;
    }

    return g_string_free(out, FALSE);
}

static void
chips_resolve_after(GPtrArray *chips, JsonArray *raw_chips)
{
    guint i;

    for (i = 0; i < chips->len; i++) {
        ClawtLrgBoardChip *chip = g_ptr_array_index(chips, i);
        JsonObject *raw = (raw_chips != NULL &&
                           i < json_array_get_length(raw_chips))
                          ? json_array_get_object_element(raw_chips, i)
                          : NULL;

        g_free(chip->after_label);
        g_free(chip->text);
        chip->after_label = chip_after_label(chips, raw_chips,
                                             array_member(raw, "after"));
        chip->text = clawt_lrg_board_chip_text(chip->initials, chip->state,
                                               chip->elapsed_label,
                                               chip->after_label);
    }
}

static ClawtLrgBoardChip *
chip_parse(JsonObject *raw)
{
    ClawtLrgBoardChip *chip;
    const gchar *worker;
    const gchar *title;

    chip = g_new0(ClawtLrgBoardChip, 1);
    worker = clawt_ipc_payload_string(raw, "worker");
    title = clawt_ipc_payload_string(raw, "title");
    chip->worker = g_strdup(worker != NULL ? worker : "?");
    chip->title = clawt_lrg_board_one_line(title != NULL ? title
                                                         : clawt_ipc_payload_string(raw, "id"),
                                           TITLE_CHARS);
    chip->initials = clawt_lrg_board_initials(worker != NULL ? worker
                                                             : chip->title);
    chip->state = g_strdup(clawt_ipc_payload_string(raw, "state"));

    if (chip->state == NULL)
        chip->state = g_strdup("?");

    chip->elapsed = clawt_ipc_payload_int(raw, "elapsed", 0);
    chip->elapsed_label = clawt_lrg_board_wait_label(chip->elapsed);
    chip->text = clawt_lrg_board_chip_text(chip->initials, chip->state,
                                           chip->elapsed_label, NULL);

    return chip;
}

static gchar *
blocks_line(JsonArray *blocks)
{
    GString *out;
    guint i;

    if (blocks == NULL || json_array_get_length(blocks) == 0)
        return NULL;

    out = g_string_new(NULL);

    for (i = 0; i < json_array_get_length(blocks); i++) {
        JsonObject *block = json_array_get_object_element(blocks, i);
        const gchar *reason = clawt_ipc_payload_string(block, "reason");

        if (reason == NULL)
            continue;

        if (out->len > 0)
            g_string_append(out, ", ");

        g_string_append(out, reason);
    }

    if (out->len == 0) {
        g_string_free(out, TRUE);
        return NULL;
    }

    return g_string_free(out, FALSE);
}

static void
add_bar(GPtrArray *bars, const gchar *task_id, const gchar *label,
        const gchar *state, gint depth, gint64 from, gint64 to,
        gint64 axis_from, gint64 axis_to)
{
    ClawtLrgBoardBar *bar;
    gdouble x0 = 0.0;
    gdouble x1 = 0.0;

    if (!clawt_lrg_board_span(axis_from, axis_to, from, to, &x0, &x1))
        return;

    bar = g_new0(ClawtLrgBoardBar, 1);
    bar->task_id = g_strdup(task_id);
    bar->label = g_strdup(label != NULL ? label : task_id);
    bar->state = g_strdup(state != NULL ? state : "?");
    bar->tone = g_strdup(clawt_lrg_board_state_tone(state));
    bar->depth = depth;
    bar->x0 = x0;
    bar->x1 = x1;
    bar->from = from;
    bar->to = to;
    g_ptr_array_add(bars, bar);
}

static void
add_timeline(GPtrArray *bars, JsonObject *raw, const gchar *task_id,
             const gchar *label, const gchar *fallback_state, gint depth,
             gint64 now, gint64 axis_from, gint64 axis_to)
{
    JsonObject *timeline = obj_member(raw, "timeline");
    JsonArray *states = array_member(timeline, "states");
    guint i;
    gboolean any = FALSE;

    for (i = 0; states != NULL && i < json_array_get_length(states); i++) {
        JsonObject *span = json_array_get_object_element(states, i);
        gint64 from = clawt_ipc_payload_int(span, "from", 0);
        gint64 to = clawt_ipc_payload_int(span, "to", 0);
        const gchar *state = clawt_ipc_payload_string(span, "state");

        if (!json_object_has_member(span, "to") || to <= 0)
            to = now;

        add_bar(bars, task_id, label, state != NULL ? state : fallback_state,
                depth, from, to, axis_from, axis_to);
        any = TRUE;
    }

    if (any)
        return;

    {
        gint64 from = clawt_ipc_payload_int(raw, "started_at", 0);
        gint64 to = clawt_ipc_payload_int(raw, "finished_at", 0);

        if (from <= 0)
            from = clawt_ipc_payload_int(raw, "created_at", axis_from);

        if (to <= 0)
            to = now;

        add_bar(bars, task_id, label, fallback_state, depth, from, to,
                axis_from, axis_to);
    }
}

static ClawtLrgBoardCard *
card_parse(JsonObject *raw, GPtrArray *bars, gint64 now,
           gint64 axis_from, gint64 axis_to)
{
    ClawtLrgBoardCard *card;
    JsonObject *ticket;
    JsonObject *review;
    JsonArray *chips;
    const gchar *title;
    guint i;

    card = g_new0(ClawtLrgBoardCard, 1);
    card->id = g_strdup(clawt_ipc_payload_string(raw, "id"));
    title = clawt_ipc_payload_string(raw, "title");
    card->title = clawt_lrg_board_one_line(title != NULL ? title : card->id,
                                           TITLE_CHARS);
    card->state = g_strdup(clawt_ipc_payload_string(raw, "state"));

    if (card->state == NULL)
        card->state = g_strdup("?");

    card->column = g_strdup(clawt_ipc_payload_string(raw, "column"));
    card->repo = g_strdup(clawt_ipc_payload_string(raw, "repo"));
    card->note = clawt_lrg_board_one_line(clawt_ipc_payload_string(raw, "note"),
                                          NOTE_CHARS);
    card->elapsed_label = clawt_lrg_board_wait_label(
        clawt_ipc_payload_int(raw, "elapsed", 0));

    if (has_double(raw, "cost_usd"))
        card->cost_label = g_strdup_printf(
            "$%.2f", json_object_get_double_member(raw, "cost_usd"));

    ticket = obj_member(raw, "ticket");

    if (ticket != NULL && clawt_ipc_payload_int(ticket, "id", 0) > 0) {
        card->ticket_label = g_strdup_printf(
            "#%" G_GINT64_FORMAT, clawt_ipc_payload_int(ticket, "id", 0));
        card->ticket_url = clawt_lrg_board_ticket_url(ticket);
    }

    review = obj_member(raw, "review");

    if (review != NULL) {
        card->review_wait_label = clawt_lrg_board_wait_label(
            clawt_ipc_payload_int(review, "wait", 0));
        card->review_for = g_strdup(clawt_ipc_payload_string(review,
                                                             "waiting_for"));
    }

    card->blocks_line = blocks_line(array_member(raw, "blocks"));
    card->quiet = clawt_ipc_payload_boolean(raw, "quiet", FALSE);
    card->chips = g_ptr_array_new_with_free_func(chip_free);
    chips = array_member(raw, "chips");

    for (i = 0; chips != NULL && i < json_array_get_length(chips); i++)
        g_ptr_array_add(card->chips,
                        chip_parse(json_array_get_object_element(chips, i)));

    chips_resolve_after(card->chips, chips);

    add_timeline(bars, raw, card->id, card->title, card->state, 0, now,
                 axis_from, axis_to);

    for (i = 0; chips != NULL && i < json_array_get_length(chips); i++) {
        JsonObject *child = json_array_get_object_element(chips, i);
        ClawtLrgBoardChip *chip = g_ptr_array_index(card->chips, i);

        add_timeline(bars, child, clawt_ipc_payload_string(child, "id"),
                     chip->title, chip->state, 1, now, axis_from, axis_to);
    }

    return card;
}

static ClawtLrgBoardColumn *
column_new(const gchar *id, const gchar *label)
{
    ClawtLrgBoardColumn *column = g_new0(ClawtLrgBoardColumn, 1);

    column->id = g_strdup(id);
    column->label = g_strdup(label != NULL ? label : id);
    column->collapsed = clawt_lrg_board_column_starts_collapsed(id);
    column->cards = g_ptr_array_new_with_free_func(card_free);

    return column;
}

static void
header_parse(ClawtLrgBoard *board, JsonObject *header)
{
    JsonObject *step;
    JsonObject *current;
    JsonObject *counts;
    const gchar *name;

    board->header.agent = g_strdup(clawt_ipc_payload_string(header, "agent"));

    if (board->header.agent == NULL)
        board->header.agent = g_strdup(board->agent);

    name = clawt_ipc_payload_string(header, "name");
    board->header.name = g_strdup(name != NULL ? name : board->header.agent);
    board->header.state = g_strdup(clawt_ipc_payload_string(header, "state"));

    if (board->header.state == NULL)
        board->header.state = g_strdup("unknown");

    board->header.team = g_strdup(clawt_ipc_payload_string(header, "team"));
    step = obj_member(header, "step");

    if (step != NULL)
        board->header.step_text = clawt_lrg_board_one_line(
            clawt_ipc_payload_string(step, "text"), TITLE_CHARS);

    current = obj_member(header, "current_task");

    if (current != NULL) {
        board->header.current_id = g_strdup(
            clawt_ipc_payload_string(current, "id"));
        board->header.current_title = clawt_lrg_board_one_line(
            clawt_ipc_payload_string(current, "title"), TITLE_CHARS);
    }

    board->header.activity_label = activity_label(header, board->now);
    board->header.mailbox = (guint)clawt_ipc_payload_int(header, "mailbox", 0);
    counts = obj_member(header, "counts");
    board->header.backlog = (guint)clawt_ipc_payload_int(counts, "backlog", 0);
    board->header.in_flight = (guint)clawt_ipc_payload_int(counts, "in_flight", 0);
    board->header.in_review = (guint)clawt_ipc_payload_int(counts, "in_review", 0);
    board->header.done = (guint)clawt_ipc_payload_int(counts, "done", 0);
    board->header.counts_line = g_strdup_printf(
        "backlog %u, in flight %u, in review %u, %s %u",
        board->header.backlog, board->header.in_flight,
        board->header.in_review,
        (board->window_spec == NULL ||
         g_strcmp0(board->window_spec, "today") == 0)
            ? "done today" : "done",
        board->header.done);
}

static ClawtLrgBoardLane *
lane_parse(JsonObject *raw, gint64 now)
{
    ClawtLrgBoardLane *lane;
    JsonObject *current;
    JsonObject *step;
    JsonObject *queue;
    const gchar *name;

    lane = g_new0(ClawtLrgBoardLane, 1);
    lane->agent = g_strdup(clawt_ipc_payload_string(raw, "agent"));
    name = clawt_ipc_payload_string(raw, "name");
    lane->name = g_strdup(name != NULL ? name : lane->agent);
    lane->state = g_strdup(clawt_ipc_payload_string(raw, "state"));

    if (lane->state == NULL)
        lane->state = g_strdup("unknown");

    current = obj_member(raw, "current_task");

    if (current != NULL) {
        lane->current_id = g_strdup(clawt_ipc_payload_string(current, "id"));
        lane->current_title = clawt_lrg_board_one_line(
            clawt_ipc_payload_string(current, "title"), TITLE_CHARS);
    }

    step = obj_member(raw, "step");

    if (step != NULL)
        lane->step_text = clawt_lrg_board_one_line(
            clawt_ipc_payload_string(step, "text"), TITLE_CHARS);

    queue = obj_member(raw, "queue");
    lane->queue_label = g_strdup_printf(
        "queued %u, running %u, review %u, mailbox %u",
        (guint)clawt_ipc_payload_int(queue, "queued", 0),
        (guint)clawt_ipc_payload_int(queue, "running", 0),
        (guint)clawt_ipc_payload_int(queue, "review", 0),
        (guint)clawt_ipc_payload_int(queue, "mailbox",
                                     clawt_ipc_payload_int(raw, "mailbox", 0)));
    lane->activity_label = activity_label(raw, now);

    return lane;
}

static void
patch_text(gchar **slot, const gchar *line)
{
    g_free(*slot);
    *slot = g_strdup(line);
}

gboolean
clawt_lrg_board_patch_step(ClawtLrgBoard *board,
                           const gchar   *agent,
                           const gchar   *line,
                           gint64         at)
{
    gboolean hit = FALSE;
    guint i;

    (void)at;

    if (board == NULL || agent == NULL || line == NULL)
        return FALSE;

    if (g_strcmp0(board->header.agent, agent) == 0) {
        patch_text(&board->header.step_text, line);
        hit = TRUE;
    }

    for (i = 0; board->lanes != NULL && i < board->lanes->len; i++) {
        ClawtLrgBoardLane *lane = g_ptr_array_index(board->lanes, i);

        if (g_strcmp0(lane->agent, agent) != 0)
            continue;

        patch_text(&lane->step_text, line);
        hit = TRUE;
    }

    return hit;
}

ClawtLrgBoardColumn *
clawt_lrg_board_column(ClawtLrgBoard *board, const gchar *id)
{
    guint i;

    if (board == NULL || board->columns == NULL)
        return NULL;

    for (i = 0; i < board->columns->len; i++) {
        ClawtLrgBoardColumn *column = g_ptr_array_index(board->columns, i);

        if (g_strcmp0(column->id, id) == 0)
            return column;
    }

    return NULL;
}

ClawtLrgBoard *
clawt_lrg_board_parse(JsonObject *raw)
{
    ClawtLrgBoard *board;
    JsonObject *window;
    JsonObject *header;
    JsonArray *columns;
    JsonArray *lanes;
    guint c;

    if (raw == NULL)
        return NULL;

    board = g_new0(ClawtLrgBoard, 1);
    board->columns = g_ptr_array_new_with_free_func(column_free);
    board->lanes = g_ptr_array_new_with_free_func(lane_free);
    board->bars = g_ptr_array_new_with_free_func(bar_free);
    board->now = clawt_ipc_payload_int(raw, "now", 0);
    board->agent = g_strdup(clawt_ipc_payload_string(raw, "agent"));
    board->team = g_strdup(clawt_ipc_payload_string(raw, "team"));
    window = obj_member(raw, "window");
    board->window_spec = g_strdup(clawt_ipc_payload_string(window, "spec"));
    board->since = clawt_ipc_payload_int(window, "since", 0);
    clawt_lrg_board_axis(board->since, board->now, board->window_spec,
                         &board->axis_from, &board->axis_to, &board->now_x);

    for (c = 0; c < G_N_ELEMENTS(column_ids); c++) {
        gint value = 0;
        const gchar *label = column_ids[c];

        if (clawt_enum_from_nick(CLAWT_TYPE_BOARD_COLUMN, column_ids[c],
                                 &value))
            label = clawt_board_column_label((ClawtBoardColumn)value);

        g_ptr_array_add(board->columns, column_new(column_ids[c], label));
    }

    columns = array_member(raw, CLAWT_BOARD_MEMBER_COLUMNS);

    for (c = 0; columns != NULL && c < json_array_get_length(columns); c++) {
        JsonObject *col = json_array_get_object_element(columns, c);
        ClawtLrgBoardColumn *dest;
        JsonArray *cards;
        const gchar *label;
        guint i;

        dest = clawt_lrg_board_column(board,
                                      clawt_ipc_payload_string(col, "id"));

        if (dest == NULL)
            continue;

        label = clawt_ipc_payload_string(col, "label");

        if (label != NULL && *label != '\0') {
            g_free(dest->label);
            dest->label = g_strdup(label);
        }

        cards = array_member(col, "cards");

        for (i = 0; cards != NULL && i < json_array_get_length(cards); i++)
            g_ptr_array_add(dest->cards,
                            card_parse(json_array_get_object_element(cards, i),
                                       board->bars, board->now,
                                       board->axis_from, board->axis_to));
    }

    header = obj_member(raw, "header");
    header_parse(board, header);
    lanes = array_member(raw, "lanes");

    for (c = 0; lanes != NULL && c < json_array_get_length(lanes); c++)
        g_ptr_array_add(board->lanes,
                        lane_parse(json_array_get_object_element(lanes, c),
                                   board->now));

    return board;
}
