/*
 * gtk-board.c - The Tasks page's Board and Timeline
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * List stays the page gtk-tasks.c already built.  Board and Timeline
 * are a second and third child of one stack, both drawn from one
 * task.board reply shaped by lrg-board-model.c.  A burst of events
 * becomes one refetch on the next idle, not one request per event.
 * turn.step rewrites the step line in place.
 */

#include "clawt-window-private.h"

#include "../lrg/clawt-lrg-board-model.h"

#define ROW_H (22.0)
#define LABEL_W (180.0)
#define BAR_PAD (8.0)

typedef struct {
    const gchar *task_id;
    const gchar *label;
    gint         depth;
    guint        first;
    guint        count;
} TimeRow;

static struct {
    ClawtWindow        *window;
    GtkStack           *stack;
    GtkWidget          *views[3];
    GtkWidget          *board_box;
    GtkWidget          *banner;
    GtkWidget          *filters;
    GtkWidget          *step;
    GtkDrawingArea     *timeline;
    GtkGesture         *timeline_click;
    AdwComboRow        *agent;
    AdwComboRow        *team;
    AdwComboRow        *window_row;
    GtkSwitch          *lanes;
    GtkSwitch          *done;
    GPtrArray          *agent_ids;
    GPtrArray          *team_ids;
    GPtrArray          *window_ids;
    ClawtLrgBoard      *model;
    GArray             *rows;
    ClawtLrgBoardView   view;
    gboolean            filling;
    gboolean            filters_ready;
    gboolean            picked;
    gboolean            fetching;
    gboolean            want_again;
    guint               idle_id;
    gchar              *force_agent;

    /* The agent page's card.  Separate reply: a different agent. */
    GtkWidget          *card;
    GtkWidget          *card_body;
    ClawtLrgBoard      *card_model;
    gboolean            card_fetching;
    gboolean            card_again;
    guint               card_idle;
    gchar              *card_agent;
} board;

static void schedule_board(ClawtWindow *self);
static void schedule_card(ClawtWindow *self);
static void redraw_board(void);
static void redraw_card(void);

static const gchar *
blank_id(AdwComboRow *row, GPtrArray *ids)
{
    const gchar *id;
    guint index;

    if (row == NULL || ids == NULL)
        return NULL;

    index = adw_combo_row_get_selected(row);

    if (index >= ids->len)
        return NULL;

    id = g_ptr_array_index(ids, index);

    if (id == NULL || *id == '\0')
        return NULL;

    return id;
}

static void
select_id(AdwComboRow *row, GPtrArray *ids, const gchar *id)
{
    guint i;

    if (row == NULL || ids == NULL)
        return;

    for (i = 0; i < ids->len; i++) {
        if (g_strcmp0(g_ptr_array_index(ids, i), id) == 0) {
            adw_combo_row_set_selected(row, i);
            return;
        }
    }

    adw_combo_row_set_selected(row, 0);
}

static void
fill_combo(AdwComboRow *row, GPtrArray *ids, JsonArray *items,
           const gchar *id_key, const gchar *label_key,
           const gchar *blank)
{
    GtkStringList *labels = gtk_string_list_new(NULL);
    g_autofree gchar *keep = g_strdup(blank_id(row, ids));
    guint i;

    g_ptr_array_set_size(ids, 0);
    gtk_string_list_append(labels, blank);
    g_ptr_array_add(ids, g_strdup(""));

    for (i = 0; items != NULL && i < json_array_get_length(items); i++) {
        JsonObject *item = json_array_get_object_element(items, i);
        const gchar *id = clawt_json_string(item, id_key, NULL);
        const gchar *label = clawt_json_string(item, label_key, id);

        if (id == NULL || *id == '\0')
            continue;

        if (label == NULL || *label == '\0')
            label = id;

        gtk_string_list_append(labels, label);
        g_ptr_array_add(ids, g_strdup(id));
    }

    board.filling = TRUE;
    adw_combo_row_set_model(row, G_LIST_MODEL(labels));
    select_id(row, ids, keep);
    board.filling = FALSE;
    g_object_unref(labels);
}

static void
ensure_filters(ClawtWindow *self)
{
    g_autoptr(JsonNode) agents = NULL;
    g_autoptr(JsonNode) teams = NULL;
    JsonObject *root;
    JsonArray *items;
    const gchar *want;

    if (board.filters_ready || board.agent == NULL)
        return;

    agents = clawt_window_request_quiet(self, "agent.list", NULL);
    teams = clawt_window_request_quiet(self, "team.list", NULL);

    if (agents == NULL && teams == NULL)
        return;

    root = clawt_payload_of(agents);
    items = (root != NULL && json_object_has_member(root, "agents"))
        ? json_object_get_array_member(root, "agents") : NULL;
    fill_combo(board.agent, board.agent_ids, items, "id", "name",
               "(team lead)");

    root = clawt_payload_of(teams);
    items = (root != NULL && json_object_has_member(root, "teams"))
        ? json_object_get_array_member(root, "teams") : NULL;
    fill_combo(board.team, board.team_ids, items, "id", "name",
               "This agent's team");

    board.filters_ready = TRUE;

    if (board.picked)
        return;

    want = board.force_agent != NULL ? board.force_agent
                                      : self->selected_agent;
    board.filling = TRUE;

    if (want != NULL && *want != '\0')
        select_id(board.agent, board.agent_ids, want);
    else if (board.agent_ids->len > 1)
        adw_combo_row_set_selected(board.agent, 1);

    board.filling = FALSE;
    board.picked = TRUE;
}

static void
mark_view(void)
{
    guint i;

    for (i = 0; i < 3; i++) {
        if (board.views[i] == NULL)
            continue;

        if (i == (guint)board.view)
            gtk_widget_add_css_class(board.views[i], "suggested-action");
        else
            gtk_widget_remove_css_class(board.views[i], "suggested-action");
    }

    if (board.filters != NULL)
        gtk_widget_set_visible(board.filters,
                               board.view != CLAWT_LRG_BOARD_VIEW_LIST);

    if (board.stack == NULL)
        return;

    if (board.view == CLAWT_LRG_BOARD_VIEW_BOARD)
        gtk_stack_set_visible_child_name(board.stack, "board");
    else if (board.view == CLAWT_LRG_BOARD_VIEW_TIMELINE)
        gtk_stack_set_visible_child_name(board.stack, "timeline");
    else
        gtk_stack_set_visible_child_name(board.stack, "list");
}

static void
tone_rgba(const gchar *tone, GdkRGBA *rgba)
{
    gdk_rgba_parse(rgba, "#777777");

    /* clawt_task_state_tone() says good, bad, warn, info or neutral. */
    if (g_strcmp0(tone, "bad") == 0 || g_strcmp0(tone, "error") == 0)
        gdk_rgba_parse(rgba, "#c0392b");
    else if (g_strcmp0(tone, "warn") == 0 || g_strcmp0(tone, "warning") == 0)
        gdk_rgba_parse(rgba, "#d68910");
    else if (g_strcmp0(tone, "good") == 0 || g_strcmp0(tone, "success") == 0)
        gdk_rgba_parse(rgba, "#1e8449");
    else if (g_strcmp0(tone, "info") == 0 || g_strcmp0(tone, "accent") == 0)
        gdk_rgba_parse(rgba, "#2471a3");
}

static void
rebuild_rows(void)
{
    guint i;

    g_clear_pointer(&board.rows, g_array_unref);

    if (board.model == NULL || board.model->bars == NULL)
        return;

    board.rows = g_array_new(FALSE, TRUE, sizeof(TimeRow));

    for (i = 0; i < board.model->bars->len; i++) {
        ClawtLrgBoardBar *bar = g_ptr_array_index(board.model->bars, i);
        TimeRow *last = NULL;
        TimeRow row;

        if (board.rows->len > 0)
            last = &g_array_index(board.rows, TimeRow,
                                  board.rows->len - 1);

        if (last != NULL && last->depth == bar->depth &&
            g_strcmp0(last->task_id, bar->task_id) == 0) {
            last->count++;
            continue;
        }

        row.task_id = bar->task_id;
        row.label = bar->label;
        row.depth = bar->depth;
        row.first = i;
        row.count = 1;
        g_array_append_val(board.rows, row);
    }
}

static void
draw_timeline(GtkDrawingArea *area, cairo_t *cr, gint width, gint height,
              gpointer user_data)
{
    GdkRGBA ink;
    guint i;
    gdouble axis_w;

    (void)area;
    (void)user_data;

    cairo_set_source_rgba(cr, 0.12, 0.12, 0.12, 1);
    cairo_paint(cr);

    if (board.model == NULL || board.rows == NULL)
        return;

    axis_w = (gdouble)width - LABEL_W - BAR_PAD;

    if (axis_w < 1.0)
        axis_w = 1.0;

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11);

    for (i = 0; i < board.rows->len; i++) {
        TimeRow *row = &g_array_index(board.rows, TimeRow, i);
        gdouble y = BAR_PAD + (gdouble)i * ROW_H;
        guint s;

        cairo_set_source_rgba(cr, 0.85, 0.85, 0.85, 1);
        cairo_move_to(cr, 6.0 + (gdouble)row->depth * 12.0, y + 14.0);
        cairo_show_text(cr, row->label != NULL ? row->label : "");

        for (s = 0; s < row->count; s++) {
            ClawtLrgBoardBar *bar = g_ptr_array_index(board.model->bars,
                                                      row->first + s);
            gdouble x0 = LABEL_W + bar->x0 * axis_w;
            gdouble x1 = LABEL_W + bar->x1 * axis_w;

            if (x1 < x0 + 2.0)
                x1 = x0 + 2.0;

            tone_rgba(bar->tone, &ink);
            gdk_cairo_set_source_rgba(cr, &ink);
            cairo_rectangle(cr, x0, y + 4.0, x1 - x0, 12.0);
            cairo_fill(cr);
        }
    }

    if (board.model->now_x >= 0.0 && board.model->now_x <= 1.0) {
        gdouble x = LABEL_W + board.model->now_x * axis_w;
        static const gdouble dash[] = { 3.0, 3.0 };

        cairo_set_source_rgba(cr, 0.95, 0.95, 0.95, 0.9);
        cairo_set_line_width(cr, 1.0);
        cairo_set_dash(cr, dash, 2, 0);
        cairo_move_to(cr, x, 0);
        cairo_line_to(cr, x, height);
        cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0);
        cairo_move_to(cr, x + 4.0, 12.0);
        cairo_show_text(cr, "now");
    }
}

static void
on_timeline_click(GtkGestureClick *gesture, gint n_press, gdouble x,
                  gdouble y, gpointer user_data)
{
    guint index;

    (void)gesture;
    (void)n_press;
    (void)x;
    (void)user_data;

    if (board.rows == NULL || board.window == NULL)
        return;

    if (y < BAR_PAD)
        return;

    index = (guint)((y - BAR_PAD) / ROW_H);

    if (index >= board.rows->len)
        return;

    clawt_gtk_show_task(board.window,
                        g_array_index(board.rows, TimeRow, index).task_id);
}

static void
sync_timeline_size(void)
{
    guint n = board.rows != NULL ? board.rows->len : 0;
    gint height = (gint)(BAR_PAD * 2.0 + (gdouble)n * ROW_H);

    if (height < 48)
        height = 48;

    if (board.timeline == NULL)
        return;

    gtk_drawing_area_set_content_width(board.timeline, 760);
    gtk_drawing_area_set_content_height(board.timeline, height);
    gtk_widget_queue_draw(GTK_WIDGET(board.timeline));
}

static void
on_open_task(GtkButton *button, gpointer user_data)
{
    const gchar *id = g_object_get_data(G_OBJECT(button), "task");

    (void)user_data;

    if (id != NULL && board.window != NULL)
        clawt_gtk_show_task(board.window, id);
}

static void
on_open_url(GtkButton *button, gpointer user_data)
{
    const gchar *url = g_object_get_data(G_OBJECT(button), "url");
    GtkWidget *root;
    g_autoptr(GtkUriLauncher) launcher = NULL;

    (void)user_data;

    if (url == NULL || board.window == NULL)
        return;

    root = GTK_WIDGET(board.window);
    launcher = gtk_uri_launcher_new(url);
    gtk_uri_launcher_launch(launcher,
                            GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL,
                            NULL, NULL, NULL);
}

static GtkWidget *
task_button(const gchar *label, const gchar *task_id)
{
    GtkWidget *button = gtk_button_new_with_label(label != NULL ? label
                                                                : "");

    gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);
    gtk_widget_set_halign(button, GTK_ALIGN_START);
    g_object_set_data_full(G_OBJECT(button), "task", g_strdup(task_id),
                           g_free);
    g_signal_connect(button, "clicked", G_CALLBACK(on_open_task), NULL);
    return button;
}

static GtkWidget *
meta_label(const gchar *text, gboolean dim)
{
    GtkWidget *label = gtk_label_new(text != NULL ? text : "");

    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_wrap(GTK_LABEL(label), FALSE);

    if (dim)
        gtk_widget_add_css_class(label, "dim-label");

    gtk_widget_add_css_class(label, "caption");
    return label;
}

static void
add_chips(GtkWidget *box, ClawtLrgBoardCard *card)
{
    guint i;

    for (i = 0; i < card->chips->len; i++) {
        ClawtLrgBoardChip *chip = g_ptr_array_index(card->chips, i);
        g_autofree gchar *line = NULL;
        GtkWidget *label;

        if (chip->after_label != NULL)
            line = g_strdup_printf("→ %s", chip->text);
        else
            line = g_strdup(chip->text);

        label = meta_label(line, FALSE);
        gtk_widget_add_css_class(label, "caption");
        gtk_box_append(GTK_BOX(box), label);
    }
}

static GtkWidget *
card_widget(ClawtLrgBoardCard *card, gboolean review)
{
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *button = gtk_button_new();
    g_autofree gchar *elapsed = NULL;

    gtk_widget_add_css_class(outer, "card");
    gtk_widget_set_margin_bottom(outer, 6);

    gtk_box_append(GTK_BOX(inner), meta_label(card->title, FALSE));

    if (card->repo != NULL)
        gtk_box_append(GTK_BOX(inner), meta_label(card->repo, TRUE));

    if (card->note != NULL && card->note[0] != '\0')
        gtk_box_append(GTK_BOX(inner), meta_label(card->note, TRUE));

    elapsed = g_strdup_printf("%s%s%s",
                              card->elapsed_label != NULL
                                  ? card->elapsed_label : "",
                              card->cost_label != NULL ? "  " : "",
                              card->cost_label != NULL
                                  ? card->cost_label : "");
    gtk_box_append(GTK_BOX(inner), meta_label(elapsed, TRUE));

    if (card->blocks_line != NULL)
        gtk_box_append(GTK_BOX(inner), meta_label(card->blocks_line, FALSE));

    if (card->quiet)
        gtk_box_append(GTK_BOX(inner), meta_label("quiet", FALSE));

    if (review && card->review_wait_label != NULL) {
        g_autofree gchar *wait = g_strdup_printf(
            "waiting %s%s%s",
            card->review_wait_label,
            card->review_for != NULL ? " for " : "",
            card->review_for != NULL ? card->review_for : "");

        gtk_box_append(GTK_BOX(inner), meta_label(wait, FALSE));
    }

    add_chips(inner, card);
    gtk_button_set_child(GTK_BUTTON(button), inner);
    g_object_set_data_full(G_OBJECT(button), "task", g_strdup(card->id),
                           g_free);
    g_signal_connect(button, "clicked", G_CALLBACK(on_open_task), NULL);
    gtk_box_append(GTK_BOX(outer), button);

    /*
     * The ticket link is a sibling of the card button.  A control
     * inside the button cannot take its own click.
     */
    if (card->ticket_url != NULL && card->ticket_label != NULL) {
        GtkWidget *link = gtk_button_new_with_label(card->ticket_label);

        gtk_widget_add_css_class(link, "flat");
        gtk_widget_set_halign(link, GTK_ALIGN_START);
        g_object_set_data_full(G_OBJECT(link), "url",
                               g_strdup(card->ticket_url), g_free);
        g_signal_connect(link, "clicked", G_CALLBACK(on_open_url), NULL);
        gtk_box_append(GTK_BOX(outer), link);
    }

    return outer;
}

static void
fill_columns(GtkWidget *host, ClawtLrgBoard *model, gboolean show_done)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    guint c;

    gtk_widget_set_margin_start(row, 12);
    gtk_widget_set_margin_end(row, 12);
    gtk_widget_set_margin_bottom(row, 12);

    for (c = 0; c < model->columns->len; c++) {
        ClawtLrgBoardColumn *column = g_ptr_array_index(model->columns, c);
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        g_autofree gchar *heading = NULL;
        GtkWidget *title;
        gboolean collapsed;
        guint i;

        heading = g_strdup_printf("%s (%u)", column->label,
                                  column->cards->len);
        title = gtk_label_new(heading);
        gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
        gtk_widget_add_css_class(title, "heading");

        if (g_strcmp0(column->id, "blocked") == 0)
            gtk_widget_add_css_class(title, "error");

        gtk_box_append(GTK_BOX(box), title);
        gtk_widget_set_size_request(box, 220, -1);
        collapsed = column->collapsed && !show_done;

        if (collapsed) {
            GtkWidget *note = meta_label("Collapsed", TRUE);

            gtk_box_append(GTK_BOX(box), note);
        } else {
            for (i = 0; i < column->cards->len; i++) {
                ClawtLrgBoardCard *card = g_ptr_array_index(column->cards, i);

                gtk_box_append(GTK_BOX(box),
                               card_widget(card,
                                           g_strcmp0(column->id,
                                                     "review") == 0));
            }
        }

        gtk_box_append(GTK_BOX(row), box);
    }

    gtk_box_append(GTK_BOX(host), row);
}

static void
fill_lanes(GtkWidget *host, ClawtLrgBoard *model)
{
    GtkWidget *box;
    guint i;

    if (model->lanes->len == 0)
        return;

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_box_append(GTK_BOX(box), meta_label("Worker lanes", FALSE));

    for (i = 0; i < model->lanes->len; i++) {
        ClawtLrgBoardLane *lane = g_ptr_array_index(model->lanes, i);
        GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        g_autofree gchar *who = NULL;

        who = g_strdup_printf("%s  %s  %s",
                              lane->name != NULL ? lane->name : lane->agent,
                              lane->state != NULL ? lane->state : "",
                              lane->activity_label != NULL
                                  ? lane->activity_label : "");
        gtk_box_append(GTK_BOX(line), meta_label(who, FALSE));

        if (lane->current_id != NULL)
            gtk_box_append(GTK_BOX(line),
                           task_button(lane->current_title, lane->current_id));

        gtk_box_append(GTK_BOX(line), meta_label(lane->queue_label, TRUE));

        if (lane->step_text != NULL)
            gtk_box_append(GTK_BOX(line), meta_label(lane->step_text, TRUE));

        gtk_box_append(GTK_BOX(box), line);
    }

    gtk_box_append(GTK_BOX(host), box);
}

static void
fill_header(GtkWidget *host, ClawtLrgBoard *model, gboolean store_step)
{
    ClawtLrgBoardHeader *header = &model->header;
    GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *step;
    g_autofree gchar *who = NULL;
    g_autofree gchar *mail = NULL;

    gtk_widget_set_margin_top(line, 8);
    gtk_widget_set_margin_start(line, 12);
    gtk_widget_set_margin_end(line, 12);

    gtk_box_append(GTK_BOX(line),
                   clawt_gtk_build_avatar(board.window->client,
                                          header->name, header->agent,
                                          FALSE, NULL, 36));

    who = g_strdup_printf("%s  %s  %s",
                          header->name != NULL ? header->name : "",
                          header->state != NULL ? header->state : "",
                          header->activity_label != NULL
                              ? header->activity_label : "");
    gtk_box_append(GTK_BOX(text), meta_label(who, FALSE));
    step = meta_label(header->step_text, TRUE);
    gtk_box_append(GTK_BOX(text), step);

    if (store_step)
        board.step = step;

    if (header->current_id != NULL)
        gtk_box_append(GTK_BOX(text),
                       task_button(header->current_title, header->current_id));

    mail = g_strdup_printf("mailbox %u", header->mailbox);
    gtk_box_append(GTK_BOX(text), meta_label(mail, TRUE));
    gtk_box_append(GTK_BOX(text), meta_label(header->counts_line, FALSE));
    gtk_box_append(GTK_BOX(line), text);
    gtk_box_append(GTK_BOX(host), line);
}

static void
set_banner(const gchar *text)
{
    if (board.banner == NULL)
        return;

    gtk_label_set_text(GTK_LABEL(board.banner), text != NULL ? text : "");
    gtk_widget_set_visible(board.banner, text != NULL && text[0] != '\0');
}

static void
redraw_board(void)
{
    gboolean lanes;
    gboolean done;

    if (board.board_box == NULL)
        return;

    board.step = NULL;
    clawt_gtk_clear_box(GTK_BOX(board.board_box));
    rebuild_rows();
    sync_timeline_size();

    if (board.model == NULL)
        return;

    lanes = board.lanes != NULL &&
        gtk_switch_get_active(board.lanes);
    done = board.done != NULL && gtk_switch_get_active(board.done);
    fill_header(board.board_box, board.model, TRUE);

    if (lanes)
        fill_lanes(board.board_box, board.model);

    fill_columns(board.board_box, board.model, done);
}

static void
redraw_card(void)
{
    if (board.card_body == NULL)
        return;

    clawt_gtk_clear_box(GTK_BOX(board.card_body));

    if (board.card_model == NULL)
        return;

    fill_header(board.card_body, board.card_model, FALSE);
}

static JsonNode *
board_payload_for(const gchar *agent, const gchar *team)
{
    const gchar *window = blank_id(board.window_row, board.window_ids);

    if (agent == NULL && team == NULL)
        return NULL;

    return clawt_ipc_payload_new("agent", agent,
                                 "team", team,
                                 "window", window,
                                 "timeline", "true",
                                 NULL);
}

static gboolean
take_reply(JsonNode *reply, const GError *error, ClawtLrgBoard **slot,
           const gchar *fallback)
{
    JsonObject *root = clawt_payload_of(reply);

    g_clear_pointer(slot, clawt_lrg_board_free);

    (void)error;
    (void)fallback;

    if (root == NULL)
        return FALSE;

    *slot = clawt_lrg_board_parse(root);
    return *slot != NULL;
}

static void
fetch_board(ClawtWindow *self)
{
    const gchar *agent;
    const gchar *team;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) reply = NULL;

    if (board.view == CLAWT_LRG_BOARD_VIEW_LIST)
        return;

    ensure_filters(self);
    agent = blank_id(board.agent, board.agent_ids);
    team = blank_id(board.team, board.team_ids);

    if (agent == NULL && team == NULL) {
        set_banner("Pick an agent, or a team with (team lead).");
        g_clear_pointer(&board.model, clawt_lrg_board_free);
        redraw_board();
        return;
    }

    reply = clawt_client_request(self->client, "task.board",
                                 board_payload_for(agent, team), &error);

    if (!take_reply(reply, error, &board.model, NULL))
        set_banner(error != NULL ? error->message
                                 : "The board did not answer.");
    else
        set_banner(NULL);

    redraw_board();
}

static void
fetch_card(ClawtWindow *self)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) reply = NULL;

    if (board.card_agent == NULL || board.card_body == NULL)
        return;

    reply = clawt_client_request(self->client, "task.board",
                                 clawt_ipc_payload_new("agent",
                                                       board.card_agent,
                                                       "timeline", "true",
                                                       NULL),
                                 &error);

    if (!take_reply(reply, error, &board.card_model, NULL) &&
        board.card_body != NULL) {
        clawt_gtk_clear_box(GTK_BOX(board.card_body));
        gtk_box_append(GTK_BOX(board.card_body),
                       meta_label(error != NULL ? error->message
                                                : "The board did not answer.",
                                  TRUE));
        return;
    }

    redraw_card();
}

static gboolean
board_idle(gpointer user_data)
{
    ClawtWindow *self = user_data;

    board.idle_id = 0;
    board.fetching = TRUE;

    do {
        board.want_again = FALSE;
        fetch_board(self);
    } while (board.want_again);

    board.fetching = FALSE;
    return G_SOURCE_REMOVE;
}

static gboolean
card_idle(gpointer user_data)
{
    ClawtWindow *self = user_data;

    board.card_idle = 0;
    board.card_fetching = TRUE;

    do {
        board.card_again = FALSE;
        fetch_card(self);
    } while (board.card_again);

    board.card_fetching = FALSE;
    return G_SOURCE_REMOVE;
}

static void
schedule_board(ClawtWindow *self)
{
    if (self == NULL || board.stack == NULL)
        return;

    if (board.view == CLAWT_LRG_BOARD_VIEW_LIST)
        return;

    if (board.fetching) {
        board.want_again = TRUE;
        return;
    }

    if (board.idle_id != 0)
        return;

    board.idle_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, board_idle,
                                    g_object_ref(self), g_object_unref);
}

static void
schedule_card(ClawtWindow *self)
{
    if (self == NULL || board.card_body == NULL || board.card_agent == NULL)
        return;

    if (board.card_fetching) {
        board.card_again = TRUE;
        return;
    }

    if (board.card_idle != 0)
        return;

    board.card_idle = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, card_idle,
                                      g_object_ref(self), g_object_unref);
}

static void
on_view(GtkButton *button, gpointer user_data)
{
    board.view = (ClawtLrgBoardView)GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(button), "view"));
    mark_view();

    if (board.view != CLAWT_LRG_BOARD_VIEW_LIST)
        schedule_board(user_data);
}

static void
on_filter(GObject *object, GParamSpec *spec, gpointer user_data)
{
    (void)object;
    (void)spec;

    if (board.filling)
        return;

    g_clear_pointer(&board.force_agent, g_free);
    schedule_board(user_data);
}

static void
on_toggle(GObject *object, GParamSpec *spec, gpointer user_data)
{
    (void)object;
    (void)spec;
    (void)user_data;

    if (board.filling)
        return;

    redraw_board();
}

static void
on_open_board(GtkButton *button, gpointer user_data)
{
    const gchar *agent = g_object_get_data(G_OBJECT(button), "agent");

    (void)user_data;
    clawt_gtk_show_agent_board(board.window, agent);
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
clawt_gtk_board_on_event(ClawtWindow *self, ClawtEvent *event)
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
            redraw_board();
        else if (board.model == NULL &&
                 board.view != CLAWT_LRG_BOARD_VIEW_LIST)
            schedule_board(self);

        if (card)
            redraw_card();
        else if (board.card_model == NULL)
            schedule_card(self);

        return;
    }

    /* The stale-event list is the library's (clawt_board_event_refreshes). */
    if (!clawt_lrg_board_event_refreshes(kind))
        return;

    if (clawt_gtk_current_page(self) == CLAWT_PAGE_TASKS &&
        board.view != CLAWT_LRG_BOARD_VIEW_LIST)
        schedule_board(self);

    if (clawt_gtk_current_page(self) == CLAWT_PAGE_AGENT)
        schedule_card(self);
}

void
clawt_gtk_board_agent_reset(ClawtWindow *self)
{
    (void)self;
    board.card = NULL;
    board.card_body = NULL;
}

void
clawt_gtk_board_mount_agent(ClawtWindow *self)
{
    GtkWidget *group = adw_preferences_group_new();
    GtkWidget *open;

    board.window = self;
    board.card = group;
    board.card_body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    g_free(board.card_agent);
    board.card_agent = g_strdup(self->selected_agent);
    g_clear_pointer(&board.card_model, clawt_lrg_board_free);

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), "Board");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        "Live state, the current task, and the column counts.");
    gtk_box_append(GTK_BOX(board.card_body),
                   meta_label("Loading the board…", TRUE));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), board.card_body);

    open = gtk_button_new_with_label("Open board");
    g_object_set_data_full(G_OBJECT(open), "agent",
                           g_strdup(self->selected_agent), g_free);
    g_signal_connect(open, "clicked", G_CALLBACK(on_open_board), NULL);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), open);
    gtk_box_append(self->inspector, group);
    schedule_card(self);
}

void
clawt_gtk_show_agent_board(ClawtWindow *self, const gchar *agent_id)
{
    board.window = self;
    g_free(board.force_agent);
    board.force_agent = g_strdup(agent_id);
    board.view = CLAWT_LRG_BOARD_VIEW_BOARD;
    board.picked = FALSE;

    if (board.agent != NULL && board.filters_ready) {
        board.filling = TRUE;
        select_id(board.agent, board.agent_ids, agent_id);
        board.filling = FALSE;
        board.picked = TRUE;
    }

    mark_view();
    clawt_gtk_show_page(self, CLAWT_PAGE_TASKS);
    schedule_board(self);
}

static GtkWidget *
combo_new(const gchar *title, GPtrArray **ids)
{
    GtkWidget *row = clawt_gtk_combo_row_new();

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    *ids = g_ptr_array_new_with_free_func(g_free);
    return row;
}

void
clawt_gtk_board_install(ClawtWindow *self, GtkWidget *outer,
                        GtkWidget *list_scroll)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *stack = gtk_stack_new();
    GtkWidget *board_scroll = gtk_scrolled_window_new();
    GtkWidget *time_scroll = gtk_scrolled_window_new();
    GtkStringList *windows = gtk_string_list_new(NULL);
    guint i;

    board.window = self;
    board.view = CLAWT_LRG_BOARD_VIEW_LIST;
    board.stack = GTK_STACK(stack);
    board.board_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    board.banner = gtk_label_new(NULL);
    board.timeline = GTK_DRAWING_AREA(gtk_drawing_area_new());

    gtk_label_set_xalign(GTK_LABEL(board.banner), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(board.banner), TRUE);
    gtk_widget_add_css_class(board.banner, "error");
    gtk_widget_set_margin_start(board.banner, 12);
    gtk_widget_set_margin_end(board.banner, 12);
    gtk_widget_set_visible(board.banner, FALSE);

    gtk_widget_set_margin_top(bar, 8);
    gtk_widget_set_margin_start(bar, 12);
    gtk_widget_set_margin_end(bar, 12);

    for (i = 0; i < clawt_lrg_board_view_count(); i++) {
        board.views[i] = gtk_button_new_with_label(
            clawt_lrg_board_view_label((ClawtLrgBoardView)i));
        g_object_set_data(G_OBJECT(board.views[i]), "view",
                          GINT_TO_POINTER((gint)i));
        g_signal_connect(board.views[i], "clicked", G_CALLBACK(on_view),
                         self);
        gtk_box_append(GTK_BOX(bar), board.views[i]);
    }

    board.agent = ADW_COMBO_ROW(combo_new("Agent", &board.agent_ids));
    board.team = ADW_COMBO_ROW(combo_new("Team", &board.team_ids));
    board.window_row = ADW_COMBO_ROW(combo_new("Window", &board.window_ids));
    gtk_string_list_append(windows, "Today");
    gtk_string_list_append(windows, "24 hours");
    gtk_string_list_append(windows, "7 days");
    g_ptr_array_add(board.window_ids, g_strdup(""));
    g_ptr_array_add(board.window_ids, g_strdup("24h"));
    g_ptr_array_add(board.window_ids, g_strdup("7d"));
    adw_combo_row_set_model(board.window_row, G_LIST_MODEL(windows));
    g_object_unref(windows);

    board.lanes = GTK_SWITCH(gtk_switch_new());
    board.done = GTK_SWITCH(gtk_switch_new());
    gtk_widget_set_valign(GTK_WIDGET(board.lanes), GTK_ALIGN_CENTER);
    gtk_widget_set_valign(GTK_WIDGET(board.done), GTK_ALIGN_CENTER);
    {
        GtkWidget *filters = gtk_list_box_new();

        board.filters = filters;
        GtkWidget *lanes_row = adw_action_row_new();
        GtkWidget *done_row = adw_action_row_new();

        gtk_list_box_set_selection_mode(GTK_LIST_BOX(filters),
                                        GTK_SELECTION_NONE);
        gtk_widget_add_css_class(filters, "boxed-list");
        gtk_widget_set_margin_start(filters, 12);
        gtk_widget_set_margin_end(filters, 12);
        gtk_widget_set_margin_top(filters, 6);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(lanes_row),
                                      "Worker lanes");
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(done_row),
                                      "Show done");
        adw_action_row_add_suffix(ADW_ACTION_ROW(lanes_row),
                                  GTK_WIDGET(board.lanes));
        adw_action_row_add_suffix(ADW_ACTION_ROW(done_row),
                                  GTK_WIDGET(board.done));
        gtk_list_box_append(GTK_LIST_BOX(filters), GTK_WIDGET(board.agent));
        gtk_list_box_append(GTK_LIST_BOX(filters), GTK_WIDGET(board.team));
        gtk_list_box_append(GTK_LIST_BOX(filters),
                            GTK_WIDGET(board.window_row));
        gtk_list_box_append(GTK_LIST_BOX(filters), lanes_row);
        gtk_list_box_append(GTK_LIST_BOX(filters), done_row);
        gtk_box_append(GTK_BOX(outer), bar);
        gtk_box_append(GTK_BOX(outer), filters);
    }

    g_signal_connect(board.agent, "notify::selected", G_CALLBACK(on_filter),
                     self);
    g_signal_connect(board.team, "notify::selected", G_CALLBACK(on_filter),
                     self);
    g_signal_connect(board.window_row, "notify::selected",
                     G_CALLBACK(on_filter), self);
    g_signal_connect(board.lanes, "notify::active", G_CALLBACK(on_toggle),
                     self);
    g_signal_connect(board.done, "notify::active", G_CALLBACK(on_toggle),
                     self);

    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(board_scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(board_scroll),
                                  board.board_box);
    gtk_drawing_area_set_draw_func(board.timeline, draw_timeline, NULL, NULL);
    board.timeline_click = gtk_gesture_click_new();
    g_signal_connect(board.timeline_click, "released",
                     G_CALLBACK(on_timeline_click), NULL);
    gtk_widget_add_controller(GTK_WIDGET(board.timeline),
                              GTK_EVENT_CONTROLLER(board.timeline_click));
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(time_scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(time_scroll),
                                  GTK_WIDGET(board.timeline));

    gtk_stack_add_named(board.stack, list_scroll, "list");
    gtk_stack_add_named(board.stack, board_scroll, "board");
    gtk_stack_add_named(board.stack, time_scroll, "timeline");
    gtk_widget_set_vexpand(stack, TRUE);
    gtk_box_append(GTK_BOX(outer), board.banner);
    gtk_box_append(GTK_BOX(outer), stack);
    mark_view();
}
