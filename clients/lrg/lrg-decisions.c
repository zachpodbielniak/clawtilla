/*
 * lrg-decisions.c - The Decisions page (mirrors clients/gtk/gtk-decisions.c)
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Choices agents are waiting on somebody for.  Its own page rather than
 * a section of the alerts panel: an alert is something that happened
 * and a decision is something that needs you.
 *
 * A policy ask ("kudu wants to run: rm -rf build -- the action rule
 * 'deletes' asks first") is a decision like any other on the wire: the
 * daemon writes the action and the matched rule into the question, so
 * the whole question is drawn as a wrapping paragraph rather than
 * clipped to a title line, which is what keeps the rule readable.
 */

#include "clawt-lrg.h"
#include "clawt-lrg-work-model.h"

typedef struct {
    ClawtLrgApp *app;
    LrgWidget   *list;
    guint        seq;
} Decisions;

static Decisions decisions;

static void refresh_list(ClawtLrgApp *app, ClawtLrgRequestFlags flags);

/* ── Answering ────────────────────────────────────────────────────── */

static void
on_settled(ClawtLrgApp *app, JsonNode *reply, const GError *error,
           gpointer user_data)
{
    if (reply == NULL)
        return;

    clawt_lrg_toast(app, user_data);
    refresh_list(app, CLAWT_LRG_QUIET);
}

/*
 * The operator answers, and the daemon routes it back to whoever asked
 * -- or, for a policy ask, releases or refuses the call waiting on it.
 */
static void
answer(LrgButton *button)
{
    const gchar *id = g_object_get_data(G_OBJECT(button), "decision-id");
    const gchar *option = g_object_get_data(G_OBJECT(button), "option");
    LrgWidget *entry = g_object_get_data(G_OBJECT(button), "entry");
    const gchar *chosen;

    chosen = clawt_lrg_decision_answer(
        option, (entry != NULL) ? clawt_lrg_entry_text(entry) : NULL);

    if (id == NULL)
        return;

    if (chosen == NULL) {
        clawt_lrg_toast(decisions.app,
                        "Type an answer, or choose one of the options");
        return;
    }

    clawt_lrg_request(decisions.app, "decision.answer",
                      clawt_ipc_payload_new("decision", id,
                                            "answer", chosen, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_settled, "Answered", NULL);
}

static void
on_answer_clicked(LrgButton *button, gpointer user_data)
{
    answer(button);
}

static void
on_answer_submitted(LrgTextInput *entry, gpointer user_data)
{
    answer(LRG_BUTTON(user_data));
}

static void
on_dismiss_clicked(LrgButton *button, gpointer user_data)
{
    const gchar *id = g_object_get_data(G_OBJECT(button), "decision-id");

    if (id == NULL)
        return;

    clawt_lrg_request(decisions.app, "decision.dismiss",
                      clawt_ipc_payload_new("decision", id, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_settled, "Dismissed", NULL);
}

/*
 * Gone from the inbox for good, open or settled.  Dismiss keeps the
 * record and says the operator passed; this is for an entry nobody wants
 * to see again.  An open one is withdrawn on the way out, so the agent
 * carries on with its default exactly as it would on a dismiss.
 */
static void
on_delete_clicked(LrgButton *button, gpointer user_data)
{
    const gchar *id = g_object_get_data(G_OBJECT(button), "decision-id");

    if (id == NULL)
        return;

    clawt_lrg_request(decisions.app, "decision.delete",
                      clawt_ipc_payload_new("decision", id, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_settled, "Deleted", NULL);
}

/* ── Drawing ──────────────────────────────────────────────────────── */

/*
 * One offered option: its sentence as a wrapping paragraph, and a
 * button beside it that chooses it.
 *
 * Not the option *as* the button: a button's label is one line, and an
 * option is a sentence -- "Re-provision clawt-oryx from a proper Fedora
 * cloud image, so the exchange mounts and the default-user config land
 * too" is a real one.  As a label it made the button as wide as the
 * sentence and took the card past the window; stacked one per line with
 * the text free to wrap, each reads as the thing it is.
 */
static LrgWidget *
decision_option_button(const gchar *id, const gchar *option, LrgWidget *entry)
{
    LrgWidget *row = clawt_lrg_hbox(10.0f);
    LrgWidget *text = clawt_lrg_paragraph(option, NULL);
    LrgWidget *pick = clawt_lrg_button("Choose", NULL,
                                       G_CALLBACK(on_answer_clicked), NULL);

    lrg_widget_add_class(row, "clawt-row");
    lrg_container_set_padding(LRG_CONTAINER(row), 8.0f);
    lrg_widget_set_fill_weight(text, 1.0f);
    lrg_widget_set_valign(text, LRG_ALIGN_CENTER);
    lrg_widget_set_valign(pick, LRG_ALIGN_CENTER);

    g_object_set_data_full(G_OBJECT(pick), "decision-id", g_strdup(id),
                           g_free);
    g_object_set_data_full(G_OBJECT(pick), "option", g_strdup(option),
                           g_free);
    g_object_set_data(G_OBJECT(pick), "entry", entry);

    clawt_lrg_take(row, text);
    clawt_lrg_take(row, pick);
    return row;
}

static LrgWidget *
decision_card(JsonObject *decision, gint64 now_us)
{
    const gchar *id = clawt_ipc_payload_string(decision, "id");
    const gchar *question = clawt_ipc_payload_string(decision, "question");
    g_autofree gchar *going = clawt_lrg_decision_going(decision);
    g_autofree gchar *meta = clawt_lrg_decision_meta(decision, now_us);
    g_auto(GStrv) options = clawt_lrg_decision_options(decision);
    LrgWidget *body = clawt_lrg_vbox(8.0f);
    LrgWidget *head = clawt_lrg_hbox(8.0f);
    LrgWidget *badges = clawt_lrg_hbox(6.0f);
    LrgWidget *typed = clawt_lrg_hbox(8.0f);
    LrgWidget *entry;
    LrgWidget *send;
    LrgWidget *drop;
    LrgWidget *remove;
    LrgWidget *closing = clawt_lrg_hbox(8.0f);
    LrgWidget *card;
    guint i;

    if (id == NULL)
        id = "";

    /* who asked and when, with the urgency beside it */
    clawt_lrg_take(head, clawt_lrg_label(meta, "caption"));
    lrg_widget_set_fill_weight(head, 1.0f);

    if (clawt_ipc_payload_boolean(decision, "urgent", FALSE))
        clawt_lrg_take(badges, clawt_lrg_badge("closing soon", "warn"));

    if (clawt_ipc_payload_boolean(decision, "settled_by_default", FALSE))
        clawt_lrg_take(badges, clawt_lrg_badge("already decided", "bad"));

    {
        LrgWidget *top = clawt_lrg_hbox(8.0f);

        clawt_lrg_take(top, head);
        clawt_lrg_take(top, badges);
        clawt_lrg_take(body, top);
    }

    clawt_lrg_take(body, clawt_lrg_paragraph((question != NULL) ? question
                                                                : "",
                                             "heading"));

    if (going != NULL)
        clawt_lrg_take(body, clawt_lrg_paragraph(going, "muted"));

    if (clawt_ipc_payload_boolean(decision, "settled_by_default", FALSE))
        clawt_lrg_take(body, clawt_lrg_paragraph(
                                 "The deadline passed, so the default has "
                                 "taken effect; answering now changes "
                                 "nothing.", "muted"));
    else if (clawt_ipc_payload_boolean(decision, "urgent", FALSE))
        clawt_lrg_take(body, clawt_lrg_paragraph(
                                 "The default stops being cheap to undo "
                                 "within a day.", "muted"));

    /*
     * The answer box and its button on one line, because they are one
     * action -- and made first, so the option buttons can read it: a
     * typed answer wins over a clicked option.
     */
    entry = clawt_lrg_entry(NULL, "\"neither, do X\" is a perfectly good "
                                  "answer");
    lrg_widget_set_fill_weight(entry, 1.0f);
    send = clawt_lrg_button("Answer", "primary",
                            G_CALLBACK(on_answer_clicked), NULL);
    g_object_set_data_full(G_OBJECT(send), "decision-id", g_strdup(id),
                           g_free);
    g_object_set_data(G_OBJECT(send), "entry", entry);
    g_signal_connect(entry, "submitted", G_CALLBACK(on_answer_submitted),
                     send);

    for (i = 0; options[i] != NULL; i++)
        clawt_lrg_take(body, decision_option_button(id, options[i], entry));

    clawt_lrg_take(typed, entry);
    clawt_lrg_take(typed, send);
    clawt_lrg_take(body, typed);

    drop = clawt_lrg_button("Does not need me", "flat",
                            G_CALLBACK(on_dismiss_clicked), NULL);
    g_object_set_data_full(G_OBJECT(drop), "decision-id", g_strdup(id),
                           g_free);

    remove = clawt_lrg_button("Delete", "danger",
                              G_CALLBACK(on_delete_clicked), NULL);
    g_object_set_data_full(G_OBJECT(remove), "decision-id", g_strdup(id),
                           g_free);

    /* the two ways of closing it, side by side and apart from answering */
    clawt_lrg_take(closing, drop);
    clawt_lrg_take(closing, remove);
    lrg_widget_set_halign(closing, LRG_ALIGN_START);
    clawt_lrg_take(body, closing);

    card = clawt_lrg_card(body);
    lrg_widget_add_class(card, "decision-card");

    if (clawt_ipc_payload_boolean(decision, "urgent", FALSE))
        lrg_widget_add_class(card, "clawt-card-selected");

    return card;
}

/*
 * The column a decision is read in: the same measure the chat has, from
 * the same resolver, so a reader who widened the conversation gets it
 * here too.  libregnum takes a number, as AdwClamp does; the width is
 * not known before layout, so the resolver answers with its reference
 * column, which is what it is documented to do for "not laid out yet".
 */
static void
apply_measure(ClawtLrgApp *app)
{
    ClawtMeasureUnit unit = CLAWT_MEASURE_DEFAULT;
    gint amount = 0;
    gint px;

    if (app->appearance != NULL) {
        unit = clawt_appearance_get_measure_unit(app->appearance);
        amount = clawt_appearance_get_measure_amount(app->appearance);
    }

    px = clawt_measure_resolve_px(unit, amount, 0, 9.0 * app->scale, 0);
    lrg_widget_set_max_size(decisions.list, (gfloat)px, -1.0f);
}

static void
on_decisions(ClawtLrgApp *app, JsonNode *reply, const GError *error,
             gpointer user_data)
{
    JsonArray *items;
    guint open = 0;
    gint64 now_us = g_get_real_time();
    guint i;

    if (GPOINTER_TO_UINT(user_data) != decisions.seq ||
        decisions.list == NULL)
        return;

    clawt_lrg_clear(decisions.list);

    if (reply == NULL) {
        clawt_lrg_take(decisions.list,
                       clawt_lrg_empty_state(
                           "Decisions are unavailable",
                           "Check the daemon connection, then reopen "
                           "Decisions to retry."));
        return;
    }

    items = clawt_lrg_decision_items(reply, &open);

    /*
     * The badge is what makes this a surface rather than a page nobody
     * opens: an agent waiting on a person has to be visible without
     * anybody thinking to look.  Set from the reply's own count, so the
     * tab and the page can never disagree after an answer here.
     */
    clawt_lrg_set_page_badge(app, CLAWT_PAGE_DECISIONS, open, "warn");

    if (items == NULL || json_array_get_length(items) == 0) {
        clawt_lrg_take(decisions.list,
                       clawt_lrg_empty_state(
                           "Nothing is waiting on you",
                           "When an agent asks for a decision -- or an "
                           "action rule asks before a tool runs -- it "
                           "appears here."));
        return;
    }

    for (i = 0; i < json_array_get_length(items); i++) {
        JsonObject *decision = json_array_get_object_element(items, i);
        LrgWidget *card;

        if (decision == NULL)
            continue;

        card = decision_card(decision, now_us);
        clawt_lrg_appear(app, card, i);
        clawt_lrg_take(decisions.list, card);
    }
}

static void
refresh_list(ClawtLrgApp *app, ClawtLrgRequestFlags flags)
{
    if (decisions.list == NULL)
        return;

    decisions.seq++;
    clawt_lrg_request(app, "decision.list", NULL, flags, on_decisions,
                      GUINT_TO_POINTER(decisions.seq), NULL);
}

static LrgWidget *
build(ClawtLrgApp *app)
{
    LrgWidget *page = clawt_lrg_page_new(
        "Decisions", "Choices agents are waiting on you for");

    decisions.app = app;
    decisions.list = clawt_lrg_vbox(12.0f);
    lrg_widget_set_halign(decisions.list, LRG_ALIGN_CENTER);
    clawt_lrg_add(clawt_lrg_page_body(page), decisions.list);
    g_object_unref(decisions.list);

    return page;
}

static void
refresh(ClawtLrgApp *app)
{
    apply_measure(app);
    refresh_list(app, CLAWT_LRG_REQUEST_NONE);
}

static void
event(ClawtLrgApp *app, ClawtEvent *ev)
{
    const gchar *kind = clawt_event_get_kind(ev);

    /*
     * Both ends: `decision.asked` and `decision.settled`.  A page that
     * heard only the first could only ever grow, and a second window,
     * the CLI or the venture bridge answering would leave this one
     * drawing an inbox that is already empty.  The tab badge is kept
     * by the shell whether or not this page is showing.
     */
    if (kind != NULL && g_str_has_prefix(kind, "decision.") &&
        app->page == CLAWT_PAGE_DECISIONS)
        refresh_list(app, CLAWT_LRG_QUIET);
}

static const ClawtLrgPage page_impl = {
    CLAWT_PAGE_DECISIONS, build, refresh, event
};

const ClawtLrgPage *
clawt_lrg_decisions_page(void)
{
    return &page_impl;
}
