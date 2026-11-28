/*
 * test-lrg-models-board.c - The agent board's rows
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Hermetic: no daemon, no display.  A canned task.board reply -- one
 * card in review with a chip that waits on its sibling, a blocked
 * card, the done column empty -- plus the span maths on its own.
 */

#include <clawtilla.h>

#include "../clients/lrg/lrg-board-model.c"

static JsonNode *
parse(const gchar *text)
{
    g_autoptr(GError) error = NULL;
    JsonNode *node = json_from_string(text, &error);

    g_assert_no_error(error);
    g_assert_nonnull(node);
    return node;
}

static const gchar *CANNED =
    "{"
    " \"agent\": \"oxpecker\","
    " \"team\": \"forge\","
    " \"now\": 1800003600,"
    " \"window\": {\"spec\": \"today\", \"since\": 1800000000},"
    " \"header\": {"
    "  \"agent\": \"oxpecker\", \"name\": \"Oxpecker\", \"state\": \"running\","
    "  \"team\": \"forge\","
    "  \"step\": {\"text\": \"Bash: make test\", \"tool\": \"Bash\", \"at\": 1800003500},"
    "  \"busy_since\": 1800003000, \"idle\": 0, \"mailbox\": 3,"
    "  \"current_task\": {\"id\": \"task-fix\", \"title\": \"Fix the export\"},"
    "  \"counts\": {\"backlog\": 1, \"planning\": 0, \"delegated\": 0,"
    "   \"review\": 1, \"gating\": 0, \"blocked\": 1, \"done\": 0,"
    "   \"in_flight\": 0, \"in_review\": 1}"
    " },"
    " \"columns\": ["
    "  {\"id\": \"blocked\", \"label\": \"Blocked\", \"cards\": ["
    "    {\"id\": \"task-stuck\", \"title\": \"Stuck on the gate\","
    "     \"state\": \"failed\", \"column\": \"blocked\","
    "     \"repo\": \"clawtilla\", \"elapsed\": 7200, \"age\": 7200,"
    "     \"note\": \"the build fell over\\nand then some more words that go on past the width of a card so the one line has to cut them\","
    "     \"quiet\": true, \"chips\": [],"
    "     \"blocks\": [{\"reason\": \"failed\"}],"
    "     \"created_at\": 1799996400, \"started_at\": 1799996400}"
    "  ]},"
    "  {\"id\": \"backlog\", \"label\": \"Backlog\", \"cards\": ["
    "    {\"id\": \"task-later\", \"title\": \"Write the note\","
    "     \"state\": \"pending\", \"column\": \"backlog\", \"position\": 1,"
    "     \"elapsed\": 0, \"age\": 60, \"chips\": []}"
    "  ]},"
    "  {\"id\": \"planning\", \"label\": \"Planning\", \"cards\": []},"
    "  {\"id\": \"delegated\", \"label\": \"Delegated\", \"cards\": []},"
    "  {\"id\": \"review\", \"label\": \"In review\", \"cards\": ["
    "    {\"id\": \"task-fix\", \"title\": \"Fix the export across a very long title that should be cut once it passes the line the card has room for on screen\","
    "     \"state\": \"running\", \"column\": \"review\", \"repo\": \"clawtilla\","
    "     \"elapsed\": 3665, \"cost_usd\": 1.25,"
    "     \"ticket\": {\"id\": 41, \"base\": \"https://v.example\", \"organization_id\": 2},"
    "     \"review\": {\"task\": \"task-child\", \"worker\": \"serval\","
    "      \"waiting_for\": \"oxpecker\", \"since\": 1800000000, \"wait\": 3600},"
    "     \"chips\": ["
    "       {\"id\": \"task-child\", \"title\": \"The export\", \"worker\": \"serval\","
    "        \"state\": \"review\", \"elapsed\": 400,"
    "        \"started_at\": 1800002000,"
    "        \"timeline\": {\"source\": \"journal\", \"states\": ["
    "          {\"state\": \"running\", \"from\": 1800002000, \"to\": 1800003000},"
    "          {\"state\": \"review\", \"from\": 1800003000}"
    "        ]}},"
    "       {\"id\": \"task-wait\", \"title\": \"The notes\", \"worker\": \"caracal\","
    "        \"state\": \"pending\", \"elapsed\": 10, \"after\": [\"task-child\"]}"
    "     ],"
    "     \"timeline\": {\"source\": \"journal\", \"states\": ["
    "       {\"state\": \"running\", \"from\": 1800001000}"
    "     ]}"
    "    }"
    "  ]},"
    "  {\"id\": \"gating\", \"label\": \"Gating\", \"cards\": []},"
    "  {\"id\": \"done\", \"label\": \"Done\", \"cards\": []}"
    " ],"
    " \"lanes\": ["
    "  {\"agent\": \"serval\", \"name\": \"Serval\", \"state\": \"running\","
    "   \"busy_since\": 1800003000, \"mailbox\": 1,"
    "   \"current_task\": {\"id\": \"task-child\", \"title\": \"The export\"},"
    "   \"queue\": {\"queued\": 2, \"running\": 1, \"review\": 1, \"mailbox\": 1}}"
    " ]"
    "}";

static void
test_columns_and_chips(void)
{
    g_autoptr(JsonNode) node = parse(CANNED);
    g_autoptr(ClawtLrgBoard) board = clawt_lrg_board_parse(
        json_node_get_object(node));
    ClawtLrgBoardColumn *blocked;
    ClawtLrgBoardColumn *review;
    ClawtLrgBoardColumn *done;
    ClawtLrgBoardCard *card;
    ClawtLrgBoardChip *chip;
    ClawtLrgBoardChip *waits;

    g_assert_nonnull(board);
    g_assert_cmpuint(board->columns->len, ==, 7);

    blocked = clawt_lrg_board_column(board, "blocked");
    g_assert_cmpuint(blocked->cards->len, ==, 1);
    card = g_ptr_array_index(blocked->cards, 0);
    g_assert_cmpstr(card->repo, ==, "clawtilla");
    g_assert_cmpstr(card->blocks_line, ==, "failed");
    g_assert_true(card->quiet);
    g_assert_nonnull(strstr(card->note, "the build fell over and then"));
    g_assert_true(g_str_has_suffix(card->note, "\xe2\x80\xa6"));

    review = clawt_lrg_board_column(board, "review");
    g_assert_cmpstr(review->label, ==, "In review");
    g_assert_cmpuint(review->cards->len, ==, 1);
    card = g_ptr_array_index(review->cards, 0);
    g_assert_cmpstr(card->elapsed_label, ==, "1h01m");
    g_assert_cmpstr(card->cost_label, ==, "$1.25");
    g_assert_cmpstr(card->ticket_label, ==, "#41");
    g_assert_cmpstr(card->ticket_url, ==,
                    "https://v.example/entity/2?back=%2Fe%2Fticket%2F41");
    g_assert_cmpstr(card->review_wait_label, ==, "1h00m");
    g_assert_cmpstr(card->review_for, ==, "oxpecker");
    g_assert_true(g_str_has_suffix(card->title, "\xe2\x80\xa6"));
    g_assert_cmpuint(card->chips->len, ==, 2);

    chip = g_ptr_array_index(card->chips, 0);
    g_assert_cmpstr(chip->initials, ==, "S");
    g_assert_cmpstr(chip->text, ==, "S review 6m");

    waits = g_ptr_array_index(card->chips, 1);
    g_assert_cmpstr(waits->initials, ==, "C");
    g_assert_cmpstr(waits->after_label, ==, "waits on The export");
    g_assert_cmpstr(waits->text, ==, "C pending 10s, waits on The export");

    done = clawt_lrg_board_column(board, "done");
    g_assert_true(done->collapsed);
    g_assert_cmpuint(done->cards->len, ==, 0);
    g_assert_false(review->collapsed);

    g_assert_cmpstr(board->header.name, ==, "Oxpecker");
    g_assert_cmpstr(board->header.state, ==, "running");
    g_assert_cmpstr(board->header.step_text, ==, "Bash: make test");
    g_assert_cmpstr(board->header.current_title, ==, "Fix the export");
    g_assert_cmpuint(board->header.mailbox, ==, 3);
    g_assert_cmpstr(board->header.counts_line, ==,
                    "backlog 1, in flight 0, in review 1, done today 0");
    g_assert_cmpuint(board->lanes->len, ==, 1);
}

static void
test_spans_and_bars(void)
{
    g_autoptr(JsonNode) node = parse(CANNED);
    g_autoptr(ClawtLrgBoard) board = clawt_lrg_board_parse(
        json_node_get_object(node));
    gdouble x0 = 1.0;
    gdouble x1 = 1.0;
    gdouble now_x = 0.0;
    gint64 from = 0;
    gint64 to = 0;
    ClawtLrgBoardBar *parent;
    ClawtLrgBoardBar *child;
    g_autofree gchar *cells = NULL;

    /* Today is midnight to the next midnight.  now is an hour in. */
    clawt_lrg_board_axis(1800000000, 1800003600, "today", &from, &to, &now_x);
    g_assert_cmpint(from, ==, 1800000000);
    g_assert_cmpint(to, ==, 1800000000 + 86400);
    g_assert_cmpfloat_with_epsilon(now_x, 3600.0 / 86400.0, 0.00001);
    g_assert_cmpfloat_with_epsilon(board->now_x, now_x, 0.00001);

    /* A window that is not today ends at now, so the mark is the edge. */
    clawt_lrg_board_axis(1800000000, 1800003600, "6h", &from, &to, &now_x);
    g_assert_cmpint(to, ==, 1800003600);
    g_assert_cmpfloat(now_x, ==, 1.0);

    g_assert_true(clawt_lrg_board_span(0, 100, 25, 75, &x0, &x1));
    g_assert_cmpfloat(x0, ==, 0.25);
    g_assert_cmpfloat(x1, ==, 0.75);

    /* Misses the axis. */
    g_assert_false(clawt_lrg_board_span(100, 200, 0, 50, &x0, &x1));
    g_assert_cmpfloat(x0, ==, 0.0);

    /* Clamped to the axis. */
    g_assert_true(clawt_lrg_board_span(100, 200, 50, 150, &x0, &x1));
    g_assert_cmpfloat(x0, ==, 0.0);
    g_assert_cmpfloat(x1, ==, 0.5);

    g_assert_cmpstr(clawt_lrg_board_wait_label(45), ==, "45s");
    g_assert_cmpstr(clawt_lrg_board_wait_label(12 * 60), ==, "12m");
    g_assert_cmpstr(clawt_lrg_board_wait_label(-4), ==, "0s");

    g_assert_cmpuint(board->bars->len, >=, 3);
    parent = g_ptr_array_index(board->bars, 0);
    g_assert_cmpstr(parent->task_id, ==, "task-stuck");
    g_assert_cmpint(parent->depth, ==, 0);

    /* The review card, then its two children.  The open review span ends at now. */
    child = NULL;
    {
        guint i;

        for (i = 0; i < board->bars->len; i++) {
            ClawtLrgBoardBar *bar = g_ptr_array_index(board->bars, i);

            if (g_strcmp0(bar->task_id, "task-child") == 0 &&
                g_strcmp0(bar->state, "review") == 0)
                child = bar;
        }
    }
    g_assert_nonnull(child);
    g_assert_cmpint(child->depth, ==, 1);
    g_assert_cmpint(child->from, ==, 1800003000);
    g_assert_cmpint(child->to, ==, 1800003600);
    g_assert_cmpstr(child->tone, ==, "info");

    cells = clawt_lrg_board_bar_cells(0.0, 0.5, 4);
    g_assert_cmpstr(cells, ==, "\xe2\x96\x88\xe2\x96\x88\xe2\x96\x91\xe2\x96\x91");
}

static void
test_chip_text_and_step(void)
{
    g_autofree gchar *plain = clawt_lrg_board_chip_text("WB", "running", "12m",
                                                        NULL);
    g_autofree gchar *linked = clawt_lrg_board_chip_text("WB", "running", "12m",
                                                         "waits on serval");
    g_autofree gchar *initials = clawt_lrg_board_initials("wilde build");
    g_autofree gchar *line = clawt_lrg_board_step_line("tool", "serval",
                                                       "no-such-tool", NULL,
                                                       "make test", FALSE);
    g_autoptr(JsonNode) node = parse(CANNED);
    g_autoptr(ClawtLrgBoard) board = clawt_lrg_board_parse(
        json_node_get_object(node));

    g_assert_cmpstr(plain, ==, "WB running 12m");
    g_assert_cmpstr(linked, ==, "WB running 12m, waits on serval");
    g_assert_cmpstr(initials, ==, "WB");
    g_assert_cmpstr(line, ==, "no-such-tool: make test");

    g_assert_true(clawt_lrg_board_event_refreshes("task.changed"));
    g_assert_true(clawt_lrg_board_event_refreshes("wait.changed"));
    g_assert_false(clawt_lrg_board_event_refreshes("turn.step"));
    g_assert_true(clawt_lrg_board_event_is_step("turn.step"));
    g_assert_false(clawt_lrg_board_event_refreshes("message"));

    g_assert_true(clawt_lrg_board_patch_step(board, "oxpecker", "Read board.h",
                                             1800003601));
    g_assert_cmpstr(board->header.step_text, ==, "Read board.h");
    g_assert_true(clawt_lrg_board_patch_step(board, "serval", "idle line", 1));
    g_assert_cmpstr(((ClawtLrgBoardLane *)g_ptr_array_index(board->lanes, 0))->step_text,
                    ==, "idle line");
    g_assert_false(clawt_lrg_board_patch_step(board, "nobody", "nope", 1));
}

static void
test_ticket_url_falls_back(void)
{
    g_autoptr(JsonNode) node = parse(
        "{\"id\": 7, \"url\": \"https://v.example/e/ticket/7\"}");
    g_autofree gchar *url = clawt_lrg_board_ticket_url(json_node_get_object(node));

    g_assert_cmpstr(url, ==, "https://v.example/e/ticket/7");
    g_assert_cmpuint(clawt_lrg_board_view_count(), ==, 3);
    g_assert_cmpstr(clawt_lrg_board_view_label(CLAWT_LRG_BOARD_VIEW_BOARD),
                    ==, "Board");
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/lrg-board/columns", test_columns_and_chips);
    g_test_add_func("/lrg-board/spans", test_spans_and_bars);
    g_test_add_func("/lrg-board/chips", test_chip_text_and_step);
    g_test_add_func("/lrg-board/ticket", test_ticket_url_falls_back);

    return g_test_run();
}
