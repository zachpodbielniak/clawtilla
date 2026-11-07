/*
 * lrg-activity.c - The Activity page: every running turn in the fleet
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Mirrors clients/gtk/gtk-activity.c.  Every turn running across the
 * fleet, from activity.list: who, where, for how long, on whose behalf,
 * and the last few things it did -- as a list and on a timeline.  Two
 * actions, each labelled with what it actually does.  Stop interrupts
 * the whole agent (agent.interrupt has no narrower form), so it says so
 * and asks first.  Steer sends a correction into the running turn
 * through msg.send, which reaches only the agent's conversation with
 * the operator, so it is offered only on that card and the others say
 * why not.
 *
 * Cards are kept by turn (agent and room) and updated in place rather
 * than rebuilt.  GTK rebuilds its rows on every step, and the web client
 * found what that costs: a steer somebody was half-way through typing
 * went with the row.  Here the entry they are typing into is the same
 * widget after a refresh as before it.
 */

#include "clawt-lrg.h"
#include "clawt-lrg-fleet-model.h"

/* How often the ages tick, and how many ticks between re-reads. */
#define TICK_SECONDS   (1)
#define REREAD_TICKS   (10)

typedef struct {
    LrgWidget *card;
    LrgWidget *title;
    LrgWidget *subtitle;
    LrgWidget *elapsed;       /* the age, beside Stop */
    LrgWidget *last_step;     /* hbox: the latest step, always visible */
    LrgWidget *expander;
    LrgWidget *steps;         /* vbox inside the expander */
    LrgWidget *timeline_slot; /* vbox inside the expander */
    LrgWidget *steer_slot;    /* vbox: the entry, or why there is none */
    LrgWidget *steer_entry;   /* nullable */
    LrgWidget *stop;
    gboolean   steerable;
    gboolean   has_steer_state;
    ClawtLrgTurn *turn;       /* the latest reading of it */
} Card;

static struct {
    ClawtLrgApp *app;
    LrgWidget   *page;
    LrgWidget   *list;
    LrgWidget   *placeholder;
    GHashTable  *cards;       /* turn key -> Card */
    GHashTable  *expanded;    /* turn keys somebody opened */
    gchar       *order;
    gint64       skew_us;     /* daemon clock minus this machine's */
    guint        ticks;
    GSource     *tick;
    gboolean     in_flight;
    gboolean     again;
} state;

static void request_activity(ClawtLrgApp *app);

/* ── Helpers ──────────────────────────────────────────────────────── */

static void
card_free(gpointer data)
{
    Card *card = data;

    g_clear_object(&card->card);
    g_clear_object(&card->title);
    g_clear_object(&card->subtitle);
    g_clear_object(&card->elapsed);
    g_clear_object(&card->last_step);
    g_clear_object(&card->expander);
    g_clear_object(&card->steps);
    g_clear_object(&card->timeline_slot);
    g_clear_object(&card->steer_slot);
    g_clear_object(&card->steer_entry);
    g_clear_object(&card->stop);
    g_clear_pointer(&card->turn, clawt_lrg_turn_free);
    g_free(card);
}

/* Now, on the daemon's clock: the ages it sends are on that clock. */
static gint64
daemon_now(void)
{
    return g_get_real_time() + state.skew_us;
}

static gboolean
page_showing(ClawtLrgApp *app)
{
    return app->page == CLAWT_PAGE_ACTIVITY;
}

/* ── Stop ─────────────────────────────────────────────────────────── */

static void
on_interrupted(ClawtLrgApp *app, JsonNode *reply, const GError *error,
               gpointer user_data)
{
    const gchar *agent_id = user_data;

    if (reply != NULL) {
        g_autofree gchar *text = clawt_lrg_activity_stop_result(agent_id,
                                                                reply);

        clawt_lrg_toast(app, text);
    }

    request_activity(app);
}

static void
on_stop_confirmed(ClawtLrgApp *app, gpointer user_data)
{
    const gchar *agent_id = user_data;

    clawt_lrg_request(app, "agent.interrupt",
                      clawt_ipc_payload_new("agent", agent_id, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_interrupted,
                      g_strdup(agent_id), g_free);
}

static void
on_stop_clicked(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    const gchar *agent_id = g_object_get_data(G_OBJECT(button), "agent-id");
    guint running = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button),
                                                       "running"));
    g_autofree gchar *heading = NULL;

    if (agent_id == NULL)
        return;

    /*
     * The question comes from the library and names how many turns go:
     * a button that does more than its card suggests must say so before
     * it does it, and say it the same way in every client.
     */
    heading = clawt_activity_stop_prompt(agent_id, running);
    clawt_lrg_confirm(app, heading,
                      "Whatever it was doing is abandoned, and anything "
                      "queued for it stays queued.",
                      "Stop it", TRUE, on_stop_confirmed,
                      g_strdup(agent_id), g_free);
}

/* ── Steer ────────────────────────────────────────────────────────── */

static void
on_steered(ClawtLrgApp *app, JsonNode *reply, const GError *error,
           gpointer user_data)
{
    LrgWidget *entry = user_data;

    if (reply == NULL)
        return;

    /*
     * Cleared only once it went: a failure keeps the words where they
     * were typed, so sending again is one keypress rather than retyping.
     * The card may have gone meanwhile (the turn ended); the entry is
     * held by this callback, so clearing it is harmless either way.
     */
    lrg_text_input_set_text(LRG_TEXT_INPUT(entry), "");

    /*
     * msg.send decides: a busy agent takes the text as a steer and says
     * "steered"; one that finished in the meantime takes it as an
     * ordinary message, read at a different moment, which is said.
     */
    clawt_lrg_toast(app, clawt_lrg_activity_steer_result(reply));
    request_activity(app);
}

static void
on_steer_submitted(LrgTextInput *entry, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    const gchar *agent_id = g_object_get_data(G_OBJECT(entry), "agent-id");
    g_autofree gchar *body =
        clawt_lrg_activity_steer_body(lrg_text_input_get_text(entry));

    if (body == NULL || agent_id == NULL)
        return;

    clawt_lrg_request(app, "msg.send",
                      clawt_ipc_payload_new("target", agent_id, "body", body,
                                            NULL),
                      CLAWT_LRG_REQUEST_NONE, on_steered,
                      g_object_ref(entry), g_object_unref);
}

static void
on_steer_clicked(LrgButton *button, gpointer user_data)
{
    LrgWidget *entry = g_object_get_data(G_OBJECT(button), "entry");

    if (entry != NULL)
        on_steer_submitted(LRG_TEXT_INPUT(entry), user_data);
}

/*
 * The steer field, or the sentence that says why there is none.
 *
 * Decided by the reply's "steerable", never by comparing rooms here:
 * whether msg.send folds into this turn is the daemon's rule, and a
 * client guessing it would offer a field whose text lands somewhere
 * else.
 */
static void
fill_steer_slot(ClawtLrgApp *app, Card *card)
{
    if (card->has_steer_state && card->steerable == card->turn->steerable)
        return;

    card->has_steer_state = TRUE;
    card->steerable = card->turn->steerable;
    clawt_lrg_clear(card->steer_slot);
    g_clear_object(&card->steer_entry);

    if (card->steerable) {
        LrgWidget *line = clawt_lrg_hbox(8.0f);
        LrgWidget *entry = clawt_lrg_entry(NULL, "Steer this turn");
        LrgWidget *send = clawt_lrg_button("Steer", "primary",
                                           G_CALLBACK(on_steer_clicked), app);

        lrg_widget_set_fill_weight(entry, 1.0f);
        g_object_set_data_full(G_OBJECT(entry), "agent-id",
                               g_strdup(card->turn->agent_id), g_free);
        g_signal_connect(entry, "submitted", G_CALLBACK(on_steer_submitted),
                         app);
        g_object_set_data(G_OBJECT(send), "entry", entry);

        card->steer_entry = g_object_ref(entry);
        clawt_lrg_take(line, entry);
        clawt_lrg_take(line, send);
        clawt_lrg_take(card->steer_slot, line);
    } else {
        LrgWidget *note = clawt_lrg_vbox(2.0f);

        clawt_lrg_take(note, clawt_lrg_label(CLAWT_LRG_STEER_REFUSAL_TITLE,
                                             "caption"));
        clawt_lrg_take(note, clawt_lrg_paragraph(CLAWT_LRG_STEER_REFUSAL_BODY,
                                                 "muted"));
        clawt_lrg_take(card->steer_slot, note);
    }
}

/* ── Steps ────────────────────────────────────────────────────────── */

/* One step: its tone as a badge, its summary beside it. */
static LrgWidget *
step_line(ClawtTurnStep *step)
{
    LrgWidget *line = clawt_lrg_hbox(8.0f);
    g_autofree gchar *summary = clawt_turn_step_summary(step);
    const gchar *tone = clawt_turn_step_tone(step);
    LrgWidget *text = clawt_lrg_label(summary, NULL);
    const gchar *kind = clawt_turn_step_is_call(step) ? "tool" : "step";

    lrg_widget_add_class(line, clawt_lrg_tone_class(tone));
    lrg_widget_set_fill_weight(text, 1.0f);
    clawt_lrg_take(line, clawt_lrg_badge(kind, tone));
    clawt_lrg_take(line, text);

    return line;
}

/*
 * The steps on a timeline: each a clip from when it began until the next
 * did, the last until now.  A list says what happened; this says where
 * the time went -- a turn stuck in one long tool call is a single wide
 * clip, which no list of summaries shows.  Locked, because it is a
 * record and not something to edit.
 */
static LrgWidget *
build_timeline(const ClawtLrgTurn *turn)
{
    g_autoptr(GPtrArray) clips = clawt_lrg_activity_clips(turn, daemon_now());
    LrgTimelineView *timeline;
    LrgTimelineViewTrack *track;
    gdouble total = 0.0;
    guint i;

    if (clips->len == 0)
        return NULL;

    timeline = lrg_timeline_view_new();
    track = lrg_timeline_view_add_track(timeline, "Steps", NULL);

    lrg_timeline_view_set_show_play_button(timeline, FALSE);
    lrg_timeline_view_set_header_width(timeline, 70.0f);
    lrg_timeline_view_set_track_height(timeline, 30.0f);

    for (i = 0; i < clips->len; i++) {
        ClawtLrgStepClip *clip = g_ptr_array_index(clips, i);

        lrg_timeline_view_add_clip(timeline, track, clip->start,
                                   clip->duration, clip->label);
        total = MAX(total, clip->start + clip->duration);
    }

    /* locked after filling: a locked track refuses new clips too */
    lrg_timeline_view_track_set_locked(track, TRUE);
    lrg_timeline_view_set_duration(timeline, MAX(total, 1.0));

    /* fit the turn into roughly a card's width */
    lrg_timeline_view_set_zoom(timeline, 560.0 / MAX(total, 1.0));
    lrg_widget_set_min_size(LRG_WIDGET(timeline), 0.0f, 92.0f);

    return LRG_WIDGET(timeline);
}

static void
fill_steps(Card *card)
{
    GPtrArray *steps = card->turn->steps;
    g_autofree gchar *heading = NULL;
    LrgWidget *timeline;
    guint i;

    clawt_lrg_clear(card->last_step);
    clawt_lrg_clear(card->steps);
    clawt_lrg_clear(card->timeline_slot);

    /* the latest step stays visible with the expander shut */
    if (steps->len > 0)
        clawt_lrg_take(card->last_step,
                       step_line(g_ptr_array_index(steps, steps->len - 1)));
    else
        clawt_lrg_take(card->last_step,
                       clawt_lrg_label("No tool call yet", "muted"));

    heading = g_strdup_printf("Last %u step%s", steps->len,
                              steps->len == 1 ? "" : "s");
    lrg_expander_set_title(LRG_EXPANDER(card->expander), heading);

    for (i = 0; i < steps->len; i++)
        clawt_lrg_take(card->steps, step_line(g_ptr_array_index(steps, i)));

    timeline = build_timeline(card->turn);

    if (timeline != NULL)
        clawt_lrg_take(card->timeline_slot, timeline);
}

/* ── Cards ────────────────────────────────────────────────────────── */

static void
on_expander_toggled(LrgExpander *expander, gboolean expanded,
                    gpointer user_data)
{
    const gchar *key = g_object_get_data(G_OBJECT(expander), "turn-key");

    /*
     * Remembered by agent and room, so a card somebody opened stays
     * open as the turn moves -- which is exactly when they are reading.
     */
    if (key == NULL)
        return;

    if (expanded)
        g_hash_table_add(state.expanded, g_strdup(key));
    else
        g_hash_table_remove(state.expanded, key);
}

static void
update_texts(Card *card)
{
    g_autofree gchar *subtitle =
        clawt_lrg_activity_subtitle(card->turn, daemon_now());

    g_autofree gchar *elapsed =
        clawt_activity_elapsed_label(card->turn->started_us, daemon_now());

    lrg_label_set_text(LRG_LABEL(card->title), card->turn->title);
    lrg_label_set_text(LRG_LABEL(card->subtitle), subtitle);
    lrg_label_set_text(LRG_LABEL(card->elapsed), elapsed);
}

static Card *
card_new(ClawtLrgApp *app, ClawtLrgTurn *turn)
{
    Card *card = g_new0(Card, 1);
    LrgWidget *content = clawt_lrg_vbox(10.0f);
    LrgWidget *header = clawt_lrg_hbox(12.0f);
    LrgWidget *titles = clawt_lrg_vbox(2.0f);
    LrgWidget *inside = clawt_lrg_vbox(10.0f);
    JsonObject *agent = clawt_lrg_find_agent(app, turn->agent_id);
    const gchar *name = clawt_ipc_payload_string(agent, "name");

    card->card = clawt_lrg_card(NULL);

    /*
     * The shimmer the sidebar uses for an agent mid-turn: every card
     * here is a turn in progress, so the page reads as alive.
     */
    lrg_widget_add_class(card->card, "clawt-busy");

    clawt_lrg_take(header, clawt_lrg_avatar(
        app, name != NULL ? name : turn->agent_id, turn->agent_id,
        clawt_ipc_payload_boolean(agent, "has_avatar", FALSE),
        clawt_ipc_payload_string(agent, "color"), 36.0f, TRUE));

    card->title = g_object_ref(clawt_lrg_label(turn->title, "heading"));
    card->subtitle = g_object_ref(clawt_lrg_label("", "muted"));
    clawt_lrg_take(titles, card->title);
    clawt_lrg_take(titles, card->subtitle);
    lrg_widget_set_fill_weight(titles, 1.0f);
    lrg_widget_set_valign(titles, LRG_ALIGN_CENTER);
    clawt_lrg_take(header, titles);

    /*
     * The age on its own, large enough to read across the room: it is
     * the number that says a turn has gone on too long.
     */
    card->elapsed = clawt_lrg_label("", "heading");
    lrg_widget_add_class(card->elapsed, clawt_lrg_tone_class("info"));
    lrg_widget_set_valign(card->elapsed, LRG_ALIGN_CENTER);
    clawt_lrg_add(header, card->elapsed);

    /*
     * "Stop agent", not "Stop": agent.interrupt ends every turn the
     * agent is running, and the label is the first place to say so.
     */
    card->stop = clawt_lrg_button("Stop agent", "danger",
                                  G_CALLBACK(on_stop_clicked), app);
    g_object_set_data_full(G_OBJECT(card->stop), "agent-id",
                           g_strdup(turn->agent_id), g_free);
    lrg_widget_set_valign(card->stop, LRG_ALIGN_CENTER);
    clawt_lrg_add(header, card->stop);
    clawt_lrg_take(content, header);

    card->last_step = clawt_lrg_hbox(8.0f);
    clawt_lrg_add(content, card->last_step);

    card->steps = clawt_lrg_vbox(6.0f);
    card->timeline_slot = clawt_lrg_vbox(0.0f);
    clawt_lrg_add(inside, card->steps);
    clawt_lrg_add(inside, card->timeline_slot);

    card->expander = LRG_WIDGET(lrg_expander_new("Steps"));
    lrg_expander_set_child(LRG_EXPANDER(card->expander), inside);
    g_object_unref(inside);
    g_object_set_data_full(G_OBJECT(card->expander), "turn-key",
                           g_strdup(turn->key), g_free);
    lrg_expander_set_expanded(LRG_EXPANDER(card->expander),
                              g_hash_table_contains(state.expanded,
                                                    turn->key));
    g_signal_connect(card->expander, "toggled",
                     G_CALLBACK(on_expander_toggled), NULL);
    clawt_lrg_add(content, card->expander);

    card->steer_slot = clawt_lrg_vbox(4.0f);
    clawt_lrg_add(content, card->steer_slot);

    clawt_lrg_take(card->card, content);
    return card;
}

/* Brings @card up to date with @turn (which it takes). */
static void
card_update(ClawtLrgApp *app, Card *card, ClawtLrgTurn *turn)
{
    clawt_lrg_turn_free(card->turn);
    card->turn = turn;

    g_object_set_data(G_OBJECT(card->stop), "running",
                      GUINT_TO_POINTER(turn->running));
    update_texts(card);
    fill_steps(card);
    fill_steer_slot(app, card);
}

/* ── The list ─────────────────────────────────────────────────────── */

static void
show_placeholder(const gchar *title, const gchar *body)
{
    clawt_lrg_clear(state.placeholder);
    clawt_lrg_take(state.placeholder, clawt_lrg_empty_state(title, body));
    lrg_widget_set_visible(state.placeholder, TRUE);
}

static void
on_activity(ClawtLrgApp *app, JsonNode *reply, const GError *error,
            gpointer user_data)
{
    g_autoptr(GPtrArray) turns = NULL;
    g_autoptr(GHashTable) seen = NULL;
    g_autofree gchar *order = NULL;
    GHashTableIter iter;
    gpointer key;
    gint64 now = 0;
    guint i;

    state.in_flight = FALSE;

    if (reply == NULL) {
        /*
         * Quiet, because this is polled -- but not silent: a page that
         * kept drawing the last answer would show turns that may have
         * ended long ago as running.
         */
        g_hash_table_remove_all(state.cards);
        clawt_lrg_clear(state.list);
        g_clear_pointer(&state.order, g_free);
        show_placeholder("Activity is unavailable",
                         "Check the daemon connection, then reopen Activity "
                         "to retry.");
        clawt_lrg_app_wake(app);
        return;
    }

    turns = clawt_lrg_activity_turns(reply, &now);
    state.skew_us = now - g_get_real_time();
    order = clawt_lrg_activity_order(turns);
    seen = g_hash_table_new(g_str_hash, g_str_equal);

    /* update what is already on screen; make what is new */
    for (i = 0; i < turns->len; i++) {
        ClawtLrgTurn *turn = g_ptr_array_index(turns, i);
        Card *card = g_hash_table_lookup(state.cards, turn->key);

        g_hash_table_add(seen, turn->key);

        if (card == NULL) {
            card = card_new(app, turn);
            g_hash_table_insert(state.cards, g_strdup(turn->key), card);
            clawt_lrg_add(state.list, card->card);
            clawt_lrg_appear(app, card->card, i);
        }

        /* the array gives the turn away; the card keeps it */
        g_ptr_array_index(turns, i) = NULL;
        card_update(app, card, turn);
    }

    /* a turn that ended takes its card with it */
    g_hash_table_iter_init(&iter, state.cards);

    while (g_hash_table_iter_next(&iter, &key, NULL)) {
        Card *card;

        if (g_hash_table_contains(seen, key))
            continue;

        card = g_hash_table_lookup(state.cards, key);
        lrg_container_remove_child(LRG_CONTAINER(state.list), card->card);
        g_hash_table_iter_remove(&iter);
    }

    /*
     * Re-ordered only when the order changed, and by moving the cards
     * that exist rather than re-adding them: removing a card with an
     * entry in it would take the keyboard focus with it.
     */
    if (g_strcmp0(order, state.order) != 0) {
        g_auto(GStrv) keys = g_strsplit(order, "\n", -1);
        guint position = 0;

        for (i = 0; keys[i] != NULL; i++) {
            Card *card = g_hash_table_lookup(state.cards, keys[i]);

            if (card == NULL)
                continue;

            lrg_container_reorder_child(LRG_CONTAINER(state.list),
                                        card->card, (gint)position);
            position++;
        }

        g_free(state.order);
        state.order = g_steal_pointer(&order);
    }

    if (g_hash_table_size(state.cards) == 0)
        show_placeholder("Nothing is running",
                         "Every agent is idle. A turn appears here the moment "
                         "one starts.");
    else
        lrg_widget_set_visible(state.placeholder, FALSE);

    {
        g_autofree gchar *subtitle = NULL;
        guint n = g_hash_table_size(state.cards);

        subtitle = (n == 0) ? g_strdup("Every turn running across the fleet")
            : g_strdup_printf("%u turn%s running", n, n == 1 ? "" : "s");
        clawt_lrg_page_set_subtitle(state.page, subtitle);
    }

    clawt_lrg_app_wake(app);

    /* an event that arrived while this was in flight asked for a re-read */
    if (state.again) {
        state.again = FALSE;
        request_activity(app);
    }
}

static void
request_activity(ClawtLrgApp *app)
{
    /*
     * One in flight at a time.  A busy fleet sends a step a second per
     * agent, and every reply rebuilds the timelines -- so the steps that
     * arrive meanwhile ask for one more read, not one each.
     */
    if (state.in_flight) {
        state.again = TRUE;
        return;
    }

    state.in_flight = TRUE;
    clawt_lrg_request(app, "activity.list", NULL, CLAWT_LRG_QUIET,
                      on_activity, NULL, NULL);
}

/*
 * Ages move whether or not anything else does: a turn sitting in one
 * long tool call produces no step for minutes, and a "running 12s" that
 * never changes reads as a stale page.  The ages tick every second from
 * the last reading, re-read every ten, and only while the page is up.
 */
static gboolean
on_tick(gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    GHashTableIter iter;
    gpointer value;

    if (!page_showing(app))
        return G_SOURCE_CONTINUE;

    g_hash_table_iter_init(&iter, state.cards);

    while (g_hash_table_iter_next(&iter, NULL, &value))
        update_texts(value);

    if (++state.ticks >= REREAD_TICKS) {
        state.ticks = 0;
        request_activity(app);
    }

    if (g_hash_table_size(state.cards) > 0)
        clawt_lrg_app_wake(app);

    return G_SOURCE_CONTINUE;
}

/* ── The page ─────────────────────────────────────────────────────── */

static void
on_refresh_clicked(LrgButton *button, gpointer user_data)
{
    request_activity(user_data);
}

static LrgWidget *
build(ClawtLrgApp *app)
{
    LrgWidget *body;

    state.app = app;
    state.cards = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                        card_free);
    state.expanded = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                           NULL);

    state.page = clawt_lrg_page_new("Activity",
                                    "Every turn running across the fleet");
    body = clawt_lrg_page_body(state.page);

    state.placeholder = clawt_lrg_vbox(0.0f);
    lrg_widget_set_visible(state.placeholder, FALSE);
    clawt_lrg_add(body, state.placeholder);
    g_object_unref(state.placeholder);

    state.list = clawt_lrg_vbox(12.0f);
    clawt_lrg_add(body, state.list);
    g_object_unref(state.list);

    clawt_lrg_take(clawt_lrg_page_toolbar(state.page),
                   clawt_lrg_button("Refresh", "flat",
                                    G_CALLBACK(on_refresh_clicked), app));

    /*
     * On the app's own context, never the ambient one (AGENTS.md: six
     * GLib APIs have attached to a loop nobody runs).  Its frame loop
     * iterates that context while it idles, so the tick fires on time.
     */
    state.tick = g_timeout_source_new_seconds(TICK_SECONDS);
    g_source_set_callback(state.tick, on_tick, app, NULL);
    clawt_lrg_app_attach(app, state.tick);

    return state.page;
}

static void
refresh(ClawtLrgApp *app)
{
    state.ticks = 0;
    request_activity(app);
}

static void
event(ClawtLrgApp *app, ClawtEvent *event)
{
    const gchar *kind = clawt_event_get_kind(event);

    if (state.page == NULL || !page_showing(app))
        return;

    /*
     * A step from any room: this page is the whole fleet.  A typing
     * frame is a turn beginning or ending somewhere, so a card comes or
     * goes; a message is usually a turn's answer.
     */
    if (g_strcmp0(kind, "turn.step") == 0 ||
        g_strcmp0(kind, "agent.typing") == 0 ||
        g_strcmp0(kind, "message") == 0)
        request_activity(app);
}

static const ClawtLrgPage page_impl = {
    CLAWT_PAGE_ACTIVITY, build, refresh, event
};

const ClawtLrgPage *
clawt_lrg_activity_page(void)
{
    return &page_impl;
}
