/*
 * lrg-board.c - The Tasks page's Board and Timeline
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The same rows as gtk-board.c, from lrg-board-model.c.  The Timeline
 * here is a text bar (clawt_lrg_board_bar_cells), one line per state
 * span, with a printed "now".  It does not draw cairo, a shared pixel
 * axis, or a line across the rows.
 *
 * Requests are async.  A burst of events is one task.board on the next
 * idle, and a reply still in flight schedules one more after it lands.
 */

#include "clawt-lrg.h"

#include "clawt-lrg-board-model.h"

#define BAR_CELLS (32)

static struct {
    ClawtLrgApp       *app;
    LrgWidget         *list_host;
    LrgWidget         *board_host;
    LrgWidget         *time_host;
    LrgWidget         *views[3];
    LrgWidget         *agent;
    LrgWidget         *agent_slot;
    LrgWidget         *team;
    LrgWidget         *team_slot;
    LrgWidget         *window_row;
    LrgWidget         *lanes;
    LrgWidget         *done;
    LrgWidget         *filters;
    ClawtLrgBoard     *model;
    ClawtLrgBoardView  view;
    gboolean           filling;
    gboolean           filters_ready;
    gboolean           fetching;
    gboolean           want_again;
    guint              idle_id;
    guint              seq;
    gchar             *force_agent;

    LrgWidget         *card;
    LrgWidget         *card_body;
    ClawtLrgBoard     *card_model;
    gchar             *card_agent;
    guint              card_gen;
    guint              card_seq;
    gboolean           card_fetching;
    gboolean           card_again;
    guint              card_idle;
} board;

static void schedule_board(ClawtLrgApp *app);
static void schedule_card(ClawtLrgApp *app);
static void draw_board(ClawtLrgApp *app);
static void draw_card(ClawtLrgApp *app);

static const gchar *
filter_id(LrgWidget *combo)
{
    const gchar *id;

    if (combo == NULL)
        return NULL;

    id = clawt_lrg_combo_id(combo);

    if (id == NULL || *id == '\0')
        return NULL;

    return id;
}

static const gchar *
badge_tone(const gchar *tone)
{
    /* clawt_task_state_tone() already returns good, bad, warn, info. */
    if (g_strcmp0(tone, "good") == 0 || g_strcmp0(tone, "success") == 0)
        return "good";

    if (g_strcmp0(tone, "bad") == 0 || g_strcmp0(tone, "error") == 0)
        return "bad";

    if (g_strcmp0(tone, "warn") == 0 || g_strcmp0(tone, "warning") == 0)
        return "warn";

    if (g_strcmp0(tone, "info") == 0 || g_strcmp0(tone, "accent") == 0)
        return "info";

    return "neutral";
}

static void
mark_view(void)
{
    guint i;

    for (i = 0; i < 3; i++) {
        if (board.views[i] == NULL)
            continue;

        if (i == (guint)board.view)
            lrg_widget_add_class(board.views[i], "primary");
        else
            lrg_widget_remove_class(board.views[i], "primary");
    }

    if (board.list_host != NULL)
        lrg_widget_set_visible(board.list_host,
                               board.view == CLAWT_LRG_BOARD_VIEW_LIST);

    if (board.board_host != NULL)
        lrg_widget_set_visible(board.board_host,
                               board.view == CLAWT_LRG_BOARD_VIEW_BOARD);

    if (board.time_host != NULL)
        lrg_widget_set_visible(board.time_host,
                               board.view == CLAWT_LRG_BOARD_VIEW_TIMELINE);

    if (board.filters != NULL)
        lrg_widget_set_visible(board.filters,
                               board.view != CLAWT_LRG_BOARD_VIEW_LIST);
}

static void
on_task(LrgButton *button, gpointer user_data)
{
    const gchar *id = g_object_get_data(G_OBJECT(button), "task");

    if (id != NULL)
        clawt_lrg_tasks_show(user_data, id);
}

static void
on_url(LrgButton *button, gpointer user_data)
{
    const gchar *url = g_object_get_data(G_OBJECT(button), "url");

    if (url != NULL)
        clawt_lrg_open_uri(user_data, url);
}

static void
on_open_board(LrgButton *button, gpointer user_data)
{
    const gchar *agent = g_object_get_data(G_OBJECT(button), "agent");

    (void)button;
    clawt_lrg_tasks_show_board(user_data, agent);
}

static LrgWidget *
task_button(ClawtLrgApp *app, const gchar *label, const gchar *task_id)
{
    LrgWidget *button = clawt_lrg_button(label != NULL ? label : "", "flat",
                                         G_CALLBACK(on_task), app);

    g_object_set_data_full(G_OBJECT(button), "task", g_strdup(task_id),
                           g_free);
    return button;
}

static LrgWidget *
face_for(ClawtLrgApp *app, const gchar *agent, const gchar *name,
         const gchar *state)
{
    JsonObject *known = clawt_lrg_find_agent(app, agent);
    gboolean has_avatar = FALSE;
    const gchar *color = NULL;

    if (known != NULL) {
        has_avatar = clawt_ipc_payload_boolean(known, "has_avatar", FALSE);
        color = clawt_ipc_payload_string(known, "color");
    }

    return clawt_lrg_avatar(app, name != NULL ? name : agent, agent,
                            has_avatar, color, 32.0f,
                            g_strcmp0(state, "running") == 0);
}

static void
add_chips(ClawtLrgApp *app, LrgWidget *box, ClawtLrgBoardCard *card)
{
    guint i;

    for (i = 0; i < card->chips->len; i++) {
        ClawtLrgBoardChip *chip = g_ptr_array_index(card->chips, i);
        g_autofree gchar *line = NULL;

        if (chip->after_label != NULL)
            line = g_strdup_printf("→ %s", chip->text);
        else
            line = g_strdup(chip->text);

        clawt_lrg_take(box, clawt_lrg_badge(line, badge_tone(chip->state)));
    }

    (void)app;
}

static void
add_card(ClawtLrgApp *app, LrgWidget *column, ClawtLrgBoardCard *card,
         gboolean review)
{
    LrgWidget *body = clawt_lrg_vbox(4.0f);
    LrgWidget *wrap;
    g_autofree gchar *elapsed = NULL;

    clawt_lrg_take(body, task_button(app, card->title, card->id));

    if (card->repo != NULL)
        clawt_lrg_take(body, clawt_lrg_paragraph(card->repo, "muted"));

    if (card->note != NULL && card->note[0] != '\0')
        clawt_lrg_take(body, clawt_lrg_one_line(card->note, "caption"));

    elapsed = g_strdup_printf("%s%s%s",
                              card->elapsed_label != NULL
                                  ? card->elapsed_label : "",
                              card->cost_label != NULL ? "  " : "",
                              card->cost_label != NULL ? card->cost_label
                                                       : "");
    clawt_lrg_take(body, clawt_lrg_paragraph(elapsed, "muted"));

    if (card->blocks_line != NULL)
        clawt_lrg_take(body, clawt_lrg_badge(card->blocks_line, "bad"));

    if (card->quiet)
        clawt_lrg_take(body, clawt_lrg_badge("quiet", "warn"));

    if (review && card->review_wait_label != NULL) {
        g_autofree gchar *wait = g_strdup_printf(
            "waiting %s%s%s",
            card->review_wait_label,
            card->review_for != NULL ? " for " : "",
            card->review_for != NULL ? card->review_for : "");

        clawt_lrg_take(body, clawt_lrg_paragraph(wait, "caption"));
    }

    add_chips(app, body, card);

    if (card->ticket_url != NULL && card->ticket_label != NULL) {
        LrgWidget *link = clawt_lrg_button(card->ticket_label, "flat",
                                           G_CALLBACK(on_url), app);

        g_object_set_data_full(G_OBJECT(link), "url",
                               g_strdup(card->ticket_url), g_free);
        clawt_lrg_take(body, link);
    }

    wrap = clawt_lrg_card(body);
    clawt_lrg_take(column, wrap);
}

static void
draw_columns(ClawtLrgApp *app, LrgWidget *host, ClawtLrgBoard *model)
{
    LrgScrollBox *scroll = lrg_scroll_box_new();
    LrgWidget *row = clawt_lrg_hbox(8.0f);
    gboolean show_done = board.done != NULL &&
        clawt_lrg_switch_active(board.done);
    guint c;

    for (c = 0; c < model->columns->len; c++) {
        ClawtLrgBoardColumn *column = g_ptr_array_index(model->columns, c);
        LrgWidget *box = clawt_lrg_vbox(4.0f);
        g_autofree gchar *heading = NULL;
        gboolean collapsed = column->collapsed && !show_done;
        guint i;

        heading = g_strdup_printf("%s (%u)", column->label,
                                  column->cards->len);
        clawt_lrg_take(box, clawt_lrg_paragraph(heading, "title"));

        if (g_strcmp0(column->id, "blocked") == 0)
            clawt_lrg_take(box, clawt_lrg_badge("needs you", "bad"));

        if (collapsed)
            clawt_lrg_take(box, clawt_lrg_paragraph("Collapsed", "muted"));
        else {
            for (i = 0; i < column->cards->len; i++)
                add_card(app, box,
                         g_ptr_array_index(column->cards, i),
                         g_strcmp0(column->id, "review") == 0);
        }

        lrg_widget_set_min_size(box, 220.0f, 0.0f);
        clawt_lrg_take(row, box);
    }

    lrg_scroll_box_set_hscroll(scroll, TRUE);
    lrg_scroll_box_set_vscroll(scroll, TRUE);
    lrg_scroll_box_set_child(scroll, row);
    lrg_widget_set_height(LRG_WIDGET(scroll), 480.0f);
    clawt_lrg_take(host, LRG_WIDGET(scroll));
}

static void
draw_lanes(ClawtLrgApp *app, LrgWidget *host, ClawtLrgBoard *model)
{
    LrgWidget *box;
    guint i;

    if (board.lanes == NULL || !clawt_lrg_switch_active(board.lanes))
        return;

    if (model->lanes->len == 0)
        return;

    box = clawt_lrg_vbox(4.0f);
    clawt_lrg_take(box, clawt_lrg_paragraph("Worker lanes", "title"));

    for (i = 0; i < model->lanes->len; i++) {
        ClawtLrgBoardLane *lane = g_ptr_array_index(model->lanes, i);
        LrgWidget *line = clawt_lrg_hbox(8.0f);
        g_autofree gchar *who = NULL;

        clawt_lrg_take(line, face_for(app, lane->agent, lane->name,
                                      lane->state));
        who = g_strdup_printf("%s  %s  %s",
                              lane->name != NULL ? lane->name : lane->agent,
                              lane->state != NULL ? lane->state : "",
                              lane->activity_label != NULL
                                  ? lane->activity_label : "");
        clawt_lrg_take(line, clawt_lrg_paragraph(who, NULL));

        if (lane->current_id != NULL)
            clawt_lrg_take(line, task_button(app, lane->current_title,
                                             lane->current_id));

        clawt_lrg_take(line, clawt_lrg_paragraph(lane->queue_label, "muted"));

        if (lane->step_text != NULL)
            clawt_lrg_take(line, clawt_lrg_one_line(lane->step_text,
                                                    "caption"));

        clawt_lrg_take(box, line);
    }

    clawt_lrg_take(host, box);
}

static void
draw_header(ClawtLrgApp *app, LrgWidget *host, ClawtLrgBoard *model)
{
    ClawtLrgBoardHeader *header = &model->header;
    LrgWidget *line = clawt_lrg_hbox(8.0f);
    LrgWidget *text = clawt_lrg_vbox(2.0f);
    g_autofree gchar *who = NULL;
    g_autofree gchar *mail = NULL;

    clawt_lrg_take(line, face_for(app, header->agent, header->name,
                                  header->state));
    who = g_strdup_printf("%s  %s  %s",
                          header->name != NULL ? header->name : "",
                          header->state != NULL ? header->state : "",
                          header->activity_label != NULL
                              ? header->activity_label : "");
    clawt_lrg_take(text, clawt_lrg_paragraph(who, NULL));

    if (header->step_text != NULL)
        clawt_lrg_take(text, clawt_lrg_one_line(header->step_text, "caption"));

    if (header->current_id != NULL)
        clawt_lrg_take(text, task_button(app, header->current_title,
                                         header->current_id));

    mail = g_strdup_printf("mailbox %u", header->mailbox);
    clawt_lrg_take(text, clawt_lrg_paragraph(mail, "muted"));
    clawt_lrg_take(text, clawt_lrg_paragraph(header->counts_line, NULL));
    clawt_lrg_take(line, text);
    clawt_lrg_take(host, line);
}

static void
draw_timeline(ClawtLrgApp *app, LrgWidget *host, ClawtLrgBoard *model)
{
    LrgWidget *box = clawt_lrg_vbox(2.0f);
    g_autofree gchar *now = NULL;
    guint mark;
    guint i;
    GString *ruler;

    mark = (guint)(model->now_x * (gdouble)BAR_CELLS);

    if (mark >= BAR_CELLS)
        mark = BAR_CELLS - 1;

    ruler = g_string_new(NULL);

    for (i = 0; i < BAR_CELLS; i++)
        g_string_append(ruler, i == mark ? "│" : "·");

    now = g_strdup_printf("now %s", ruler->str);
    g_string_free(ruler, TRUE);
    clawt_lrg_take(box, clawt_lrg_paragraph(now, "muted"));

    for (i = 0; i < model->bars->len; i++) {
        ClawtLrgBoardBar *bar = g_ptr_array_index(model->bars, i);
        g_autofree gchar *cells = clawt_lrg_board_bar_cells(bar->x0, bar->x1,
                                                            BAR_CELLS);
        g_autofree gchar *title = g_strdup_printf("%*s%s",
                                                  bar->depth * 2, "",
                                                  bar->label != NULL
                                                      ? bar->label : "");
        LrgWidget *row = clawt_lrg_hbox(6.0f);

        clawt_lrg_take(row, clawt_lrg_paragraph(cells, "caption"));
        clawt_lrg_take(row, clawt_lrg_badge(bar->state != NULL ? bar->state
                                                               : "",
                                            badge_tone(bar->tone)));
        clawt_lrg_take(row, task_button(app, title, bar->task_id));
        clawt_lrg_take(box, row);
    }

    clawt_lrg_take(host, box);
}

static void
draw_board(ClawtLrgApp *app)
{
    if (board.board_host == NULL || board.time_host == NULL)
        return;

    clawt_lrg_clear(board.board_host);
    clawt_lrg_clear(board.time_host);

    if (board.model == NULL)
        return;

    draw_header(app, board.board_host, board.model);
    draw_lanes(app, board.board_host, board.model);
    draw_columns(app, board.board_host, board.model);
    draw_timeline(app, board.time_host, board.model);
}

static void
draw_card(ClawtLrgApp *app)
{
    if (board.card_body == NULL)
        return;

    clawt_lrg_clear(board.card_body);

    if (board.card_model == NULL) {
        clawt_lrg_take(board.card_body,
                       clawt_lrg_paragraph("No board yet.", "muted"));
        return;
    }

    draw_header(app, board.card_body, board.card_model);
}

static JsonNode *
payload_for(const gchar *agent, const gchar *team)
{
    const gchar *window = filter_id(board.window_row);

    if (agent == NULL && team == NULL)
        return NULL;

    return clawt_ipc_payload_new("agent", agent,
                                 "team", team,
                                 "window", window,
                                 "timeline", "true",
                                 NULL);
}

static void
on_board(ClawtLrgApp *app, JsonNode *reply, const GError *error,
         gpointer user_data)
{
    JsonObject *root = clawt_ipc_reply_object(reply);

    if (GPOINTER_TO_UINT(user_data) != board.seq)
        return;

    board.fetching = FALSE;
    g_clear_pointer(&board.model, clawt_lrg_board_free);

    if (root == NULL) {
        clawt_lrg_clear(board.board_host);
        clawt_lrg_take(board.board_host,
                       clawt_lrg_paragraph(error != NULL
                                              ? error->message
                                              : "The board did not answer.",
                                          "caption"));
    } else {
        board.model = clawt_lrg_board_parse(root);
        draw_board(app);
    }

    if (board.want_again) {
        board.want_again = FALSE;
        schedule_board(app);
    }
}

static void
on_card(ClawtLrgApp *app, JsonNode *reply, const GError *error,
        gpointer user_data)
{
    JsonObject *root = clawt_ipc_reply_object(reply);
    guint gen = GPOINTER_TO_UINT(user_data);

    board.card_fetching = FALSE;

    if (gen != board.card_gen || board.card_body == NULL) {
        if (board.card_again) {
            board.card_again = FALSE;
            schedule_card(app);
        }

        return;
    }

    g_clear_pointer(&board.card_model, clawt_lrg_board_free);

    if (root == NULL) {
        clawt_lrg_clear(board.card_body);
        clawt_lrg_take(board.card_body,
                       clawt_lrg_paragraph(error != NULL
                                              ? error->message
                                              : "The board did not answer.",
                                          "muted"));
    } else {
        board.card_model = clawt_lrg_board_parse(root);
        draw_card(app);
    }

    if (board.card_again) {
        board.card_again = FALSE;
        schedule_card(app);
    }
}

static gboolean
board_idle(gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    const gchar *agent;
    const gchar *team;
    JsonNode *payload;

    board.idle_id = 0;

    if (board.view == CLAWT_LRG_BOARD_VIEW_LIST || board.board_host == NULL)
        return G_SOURCE_REMOVE;

    agent = filter_id(board.agent);
    team = filter_id(board.team);
    payload = payload_for(agent, team);

    if (payload == NULL) {
        clawt_lrg_clear(board.board_host);
        clawt_lrg_take(board.board_host,
                       clawt_lrg_paragraph(
                           "Pick an agent, or a team with (team lead).",
                           "caption"));
        return G_SOURCE_REMOVE;
    }

    board.seq++;
    board.fetching = TRUE;
    clawt_lrg_request(app, "task.board", payload, CLAWT_LRG_QUIET, on_board,
                      GUINT_TO_POINTER(board.seq), NULL);
    return G_SOURCE_REMOVE;
}

static gboolean
card_idle(gpointer user_data)
{
    ClawtLrgApp *app = user_data;

    board.card_idle = 0;

    if (board.card_agent == NULL || board.card_body == NULL)
        return G_SOURCE_REMOVE;

    board.card_seq = board.card_gen;
    board.card_fetching = TRUE;
    clawt_lrg_request(app, "task.board",
                      clawt_ipc_payload_new("agent", board.card_agent,
                                            "timeline", "true", NULL),
                      CLAWT_LRG_QUIET, on_card,
                      GUINT_TO_POINTER(board.card_seq), NULL);
    return G_SOURCE_REMOVE;
}

static void
schedule_board(ClawtLrgApp *app)
{
    if (app == NULL || board.view == CLAWT_LRG_BOARD_VIEW_LIST)
        return;

    if (board.fetching) {
        board.want_again = TRUE;
        return;
    }

    if (board.idle_id != 0)
        return;

    board.idle_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, board_idle, app,
                                    NULL);
}

static void
schedule_card(ClawtLrgApp *app)
{
    if (app == NULL || board.card_body == NULL || board.card_agent == NULL)
        return;

    if (board.card_fetching) {
        board.card_again = TRUE;
        return;
    }

    if (board.card_idle != 0)
        return;

    board.card_idle = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, card_idle, app,
                                      NULL);
}

static void
on_view(LrgButton *button, gpointer user_data)
{
    board.view = (ClawtLrgBoardView)GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(button), "view"));
    mark_view();

    if (board.view != CLAWT_LRG_BOARD_VIEW_LIST)
        schedule_board(user_data);
}

static void
on_filter(GObject *object, gpointer user_data)
{
    (void)object;

    if (board.filling)
        return;

    g_clear_pointer(&board.force_agent, g_free);
    schedule_board(user_data);
}

static void
on_toggle(GObject *object, gpointer user_data)
{
    (void)object;

    if (board.filling)
        return;

    draw_board(user_data);
}

static void
combo_lists(JsonArray *items, const gchar *id_key, const gchar *label_key,
            const gchar *blank, gchar ***ids_out, gchar ***labels_out)
{
    GPtrArray *ids = g_ptr_array_new();
    GPtrArray *labels = g_ptr_array_new();
    guint i;

    g_ptr_array_add(ids, g_strdup(""));
    g_ptr_array_add(labels, g_strdup(blank));

    for (i = 0; items != NULL && i < json_array_get_length(items); i++) {
        JsonObject *item = json_array_get_object_element(items, i);
        const gchar *id = clawt_ipc_payload_string(item, id_key);
        const gchar *label = clawt_ipc_payload_string(item, label_key);

        if (id == NULL || *id == '\0')
            continue;

        if (label == NULL || *label == '\0')
            label = id;

        g_ptr_array_add(ids, g_strdup(id));
        g_ptr_array_add(labels, g_strdup(label));
    }

    g_ptr_array_add(ids, NULL);
    g_ptr_array_add(labels, NULL);
    *ids_out = (gchar **)g_ptr_array_free(ids, FALSE);
    *labels_out = (gchar **)g_ptr_array_free(labels, FALSE);
}

static void
install_combo(LrgWidget **where, LrgWidget *slot, gchar **ids, gchar **labels,
              const gchar *keep, ClawtLrgApp *app)
{
    LrgWidget *combo;

    board.filling = TRUE;
    clawt_lrg_clear(slot);
    combo = clawt_lrg_combo((const gchar *const *)ids,
                            (const gchar *const *)labels, keep);
    g_signal_connect(combo, "changed", G_CALLBACK(on_filter), app);
    clawt_lrg_add(slot, combo);
    *where = combo;
    g_object_unref(combo);
    board.filling = FALSE;
    g_strfreev(ids);
    g_strfreev(labels);
}

static void
on_agents(ClawtLrgApp *app, JsonNode *reply, const GError *error,
          gpointer user_data)
{
    JsonObject *root = clawt_ipc_reply_object(reply);
    JsonArray *items;
    gchar **ids = NULL;
    gchar **labels = NULL;
    const gchar *keep;

    (void)error;
    (void)user_data;

    if (board.agent_slot == NULL || root == NULL)
        return;

    items = json_object_has_member(root, "agents")
        ? json_object_get_array_member(root, "agents") : NULL;
    keep = board.force_agent != NULL ? board.force_agent
                                      : filter_id(board.agent);

    if (keep == NULL && app->agent_id != NULL)
        keep = app->agent_id;

    combo_lists(items, "id", "name", "(team lead)", &ids, &labels);
    install_combo(&board.agent, board.agent_slot, ids, labels,
                  keep != NULL ? keep : "", app);

    if (board.view != CLAWT_LRG_BOARD_VIEW_LIST)
        schedule_board(app);
}

static void
on_teams(ClawtLrgApp *app, JsonNode *reply, const GError *error,
         gpointer user_data)
{
    JsonObject *root = clawt_ipc_reply_object(reply);
    JsonArray *items;
    gchar **ids = NULL;
    gchar **labels = NULL;
    const gchar *keep;

    (void)error;
    (void)user_data;

    if (board.team_slot == NULL || root == NULL)
        return;

    items = json_object_has_member(root, "teams")
        ? json_object_get_array_member(root, "teams") : NULL;
    keep = filter_id(board.team);
    combo_lists(items, "id", "name", "This agent's team", &ids, &labels);
    install_combo(&board.team, board.team_slot, ids, labels,
                  keep != NULL ? keep : "", app);
}

static void
ensure_filters(ClawtLrgApp *app)
{
    if (board.filters_ready)
        return;

    board.filters_ready = TRUE;
    clawt_lrg_request(app, "agent.list", NULL, CLAWT_LRG_QUIET, on_agents,
                      NULL, NULL);
    clawt_lrg_request(app, "team.list", NULL, CLAWT_LRG_QUIET, on_teams,
                      NULL, NULL);
}

static gchar *
step_from_event(ClawtEvent *event)
{
    return clawt_lrg_board_step_line(
        clawt_event_get_detail(event, CLAWT_STEP_MEMBER_KIND),
        clawt_event_get_subject(event),
        clawt_event_get_detail(event, CLAWT_STEP_MEMBER_TOOL),
        clawt_event_get_detail(event, CLAWT_STEP_MEMBER_TEXT),
        clawt_event_get_detail(event, CLAWT_STEP_MEMBER_DETAIL),
        clawt_event_get_detail_int(event, CLAWT_STEP_MEMBER_FAILED) != 0);
}

void
clawt_lrg_board_on_event(ClawtLrgApp *app, ClawtEvent *event)
{
    const gchar *kind = clawt_event_get_kind(event);

    if (kind == NULL)
        return;

    if (g_strcmp0(kind, "agent.created") == 0 ||
        g_strcmp0(kind, "agent.removed") == 0)
        board.filters_ready = FALSE;

    if (clawt_lrg_board_event_is_step(kind)) {
        g_autofree gchar *line = step_from_event(event);
        const gchar *agent = clawt_event_get_subject(event);
        gint64 at = clawt_event_get_detail_int(event, CLAWT_STEP_MEMBER_TS);
        gboolean page = FALSE;
        gboolean card = FALSE;

        if (board.model != NULL)
            page = clawt_lrg_board_patch_step(board.model, agent, line, at);

        if (board.card_model != NULL)
            card = clawt_lrg_board_patch_step(board.card_model, agent, line,
                                              at);

        if (page)
            draw_board(app);
        else if (board.model == NULL &&
                 board.view != CLAWT_LRG_BOARD_VIEW_LIST &&
                 app->page == CLAWT_PAGE_TASKS)
            schedule_board(app);

        if (card)
            draw_card(app);
        else if (board.card_model == NULL && app->page == CLAWT_PAGE_AGENT)
            schedule_card(app);

        return;
    }

    if (!clawt_lrg_board_event_refreshes(kind))
        return;

    if (app->page == CLAWT_PAGE_TASKS &&
        board.view != CLAWT_LRG_BOARD_VIEW_LIST) {
        ensure_filters(app);
        schedule_board(app);
    }

    if (app->page == CLAWT_PAGE_AGENT)
        schedule_card(app);
}

void
clawt_lrg_board_agent_reset(void)
{
    board.card_gen++;
    board.card = NULL;
    board.card_body = NULL;
}

void
clawt_lrg_board_mount_agent(ClawtLrgApp *app, LrgWidget *body,
                            const gchar *agent_id)
{
    LrgWidget *inner = clawt_lrg_vbox(4.0f);
    LrgWidget *open;

    clawt_lrg_board_agent_reset();
    board.app = app;
    g_free(board.card_agent);
    board.card_agent = g_strdup(agent_id);
    g_clear_pointer(&board.card_model, clawt_lrg_board_free);
    board.card_body = inner;
    clawt_lrg_take(inner, clawt_lrg_paragraph("Loading the board…", "muted"));
    open = clawt_lrg_button("Open board", "primary",
                            G_CALLBACK(on_open_board), app);
    g_object_set_data_full(G_OBJECT(open), "agent", g_strdup(agent_id),
                           g_free);
    clawt_lrg_take(inner, open);
    board.card = clawt_lrg_card(inner);
    clawt_lrg_take(body, board.card);
    schedule_card(app);
}

void
clawt_lrg_tasks_show_board(ClawtLrgApp *app, const gchar *agent_id)
{
    board.app = app;
    g_free(board.force_agent);
    board.force_agent = g_strdup(agent_id);
    board.view = CLAWT_LRG_BOARD_VIEW_BOARD;
    board.filters_ready = FALSE;
    clawt_lrg_show_page(app, CLAWT_PAGE_TASKS);
    mark_view();
    ensure_filters(app);
    schedule_board(app);
}

LrgWidget *
clawt_lrg_board_install(ClawtLrgApp *app, LrgWidget *body)
{
    LrgWidget *bar = clawt_lrg_hbox(6.0f);
    const gchar *window_ids[] = { "", "24h", "7d", NULL };
    const gchar *window_labels[] = { "Today", "24 hours", "7 days", NULL };
    guint i;

    board.app = app;
    board.view = CLAWT_LRG_BOARD_VIEW_LIST;
    board.list_host = clawt_lrg_vbox(8.0f);
    board.board_host = clawt_lrg_vbox(8.0f);
    board.time_host = clawt_lrg_vbox(4.0f);
    board.filters = clawt_lrg_vbox(4.0f);
    board.agent_slot = clawt_lrg_vbox(0.0f);
    board.team_slot = clawt_lrg_vbox(0.0f);
    board.lanes = clawt_lrg_switch("Worker lanes", FALSE);
    board.done = clawt_lrg_switch("Show done", FALSE);
    board.window_row = clawt_lrg_combo(window_ids, window_labels, "");

    for (i = 0; i < clawt_lrg_board_view_count(); i++) {
        board.views[i] = clawt_lrg_button(
            clawt_lrg_board_view_label((ClawtLrgBoardView)i), NULL,
            G_CALLBACK(on_view), app);
        g_object_set_data(G_OBJECT(board.views[i]), "view",
                          GINT_TO_POINTER((gint)i));
        clawt_lrg_take(bar, board.views[i]);
    }

    g_signal_connect(board.window_row, "changed", G_CALLBACK(on_filter), app);
    g_signal_connect(board.lanes, "toggled", G_CALLBACK(on_toggle), app);
    g_signal_connect(board.done, "toggled", G_CALLBACK(on_toggle), app);
    clawt_lrg_take(board.filters, board.agent_slot);
    clawt_lrg_take(board.filters, board.team_slot);
    clawt_lrg_take(board.filters, board.window_row);
    clawt_lrg_take(board.filters, board.lanes);
    clawt_lrg_take(board.filters, board.done);

    clawt_lrg_take(body, bar);
    clawt_lrg_take(body, board.filters);
    clawt_lrg_take(body, board.list_host);
    clawt_lrg_take(body, board.board_host);
    clawt_lrg_take(body, board.time_host);
    mark_view();
    return board.list_host;
}
