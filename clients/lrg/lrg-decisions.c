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
 *
 * A skill an agent proposed (clawtilla_skill_propose) is a decision too,
 * recognised by the library's own id rule, and is drawn as a review
 * rather than a question: the agent's reason, the scan's warnings, the
 * diff or the whole proposed SKILL.md as plain monospace text, and
 * Approve / Reject / Edit, which send `skill.proposal_resolve`.
 */

#include "clawt-lrg.h"
#include "clawt-lrg-work-model.h"

typedef struct {
    ClawtLrgApp          *app;
    LrgWidget            *list;
    guint                 seq;
    ClawtLrgRequestFlags  flags;       /* the refresh's, for decision.list */
    JsonNode             *proposals;   /* the last skill.proposals reply */
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

/* ── Skill proposals ──────────────────────────────────────────────── */

/*
 * Approve or Reject.  The note entry is read at click time and is
 * optional: words for the agent, most useful on a rejection.
 */
static void
on_proposal_action_clicked(LrgButton *button, gpointer user_data)
{
    const gchar *id = g_object_get_data(G_OBJECT(button), "decision-id");
    LrgWidget *note = g_object_get_data(G_OBJECT(button), "entry");
    const gchar *action = user_data;
    g_autofree gchar *refusal = NULL;
    JsonNode *payload = clawt_lrg_proposal_resolve_payload(
        id, action, (note != NULL) ? clawt_lrg_entry_text(note) : NULL,
        NULL, NULL, &refusal);

    if (payload == NULL) {
        clawt_lrg_toast(decisions.app, refusal);
        return;
    }

    clawt_lrg_request(decisions.app, "skill.proposal_resolve", payload,
                      CLAWT_LRG_REQUEST_NONE, on_settled,
                      (gpointer)(g_strcmp0(action, "approve") == 0
                                     ? "Approved" : "Rejected"),
                      NULL);
}

/* The edit form's fields, and the proposal they belong to. */
typedef struct {
    gchar     *id;
    LrgWidget *description;   /* borrowed: the dialog's content holds them */
    LrgWidget *body;
} ProposalEdit;

static void
proposal_edit_free(gpointer data)
{
    ProposalEdit *edit = data;

    g_free(edit->id);
    g_free(edit);
}

static gboolean
on_proposal_edit_response(ClawtLrgApp *app, LrgDialog *dialog,
                          gint response, gpointer user_data)
{
    ProposalEdit *edit = user_data;
    g_autofree gchar *body = clawt_lrg_editor_text(edit->body);
    g_autofree gchar *refusal = NULL;
    JsonNode *payload = clawt_lrg_proposal_resolve_payload(
        edit->id, "edit", NULL, clawt_lrg_entry_text(edit->description),
        body, &refusal);

    /* refused here: the dialog stays up with what was typed */
    if (payload == NULL) {
        clawt_lrg_toast(app, refusal);
        return FALSE;
    }

    clawt_lrg_request(app, "skill.proposal_resolve", payload,
                      CLAWT_LRG_REQUEST_NONE, on_settled,
                      (gpointer)"Edited and approved", NULL);
    return TRUE;
}

/*
 * The proposed text, to change before it is approved.  Prefilled with
 * what the daemon sent, which is the escaped form: a forged marker the
 * agent wrote is visibly rewritten in the editor rather than approved
 * back in unseen.
 */
static void
on_proposal_edit_clicked(LrgButton *button, gpointer user_data)
{
    const gchar *id = g_object_get_data(G_OBJECT(button), "decision-id");
    JsonObject *proposal = clawt_lrg_proposal_find(decisions.proposals, id);
    LrgWidget *form;
    ProposalEdit *edit;

    if (proposal == NULL)
        return;

    form = clawt_lrg_form_new();
    edit = g_new0(ProposalEdit, 1);
    edit->id = g_strdup(id);
    edit->description = clawt_lrg_entry(
        clawt_ipc_payload_string(proposal, "description"), "Use this when …");
    edit->body = clawt_lrg_editor(clawt_ipc_payload_string(proposal, "body"),
                                  NULL);
    clawt_lrg_form_add(form, "Description", edit->description,
                       "The only part an agent reads before opening it.");
    clawt_lrg_form_add(form, "Body", edit->body, NULL);

    g_object_unref(clawt_lrg_dialog(decisions.app, "Edit the proposed skill",
                     "Your version is written and approved; the agent is "
                     "told it was edited.",
                     form, "Save and approve", FALSE,
                     on_proposal_edit_response, edit, proposal_edit_free));
    g_object_unref(form);
}

static LrgWidget *
proposal_button(const gchar *label, const gchar *style, const gchar *id,
                GCallback on_clicked, gpointer user_data, LrgWidget *entry)
{
    LrgWidget *button = clawt_lrg_button(label, style, on_clicked, user_data);

    g_object_set_data_full(G_OBJECT(button), "decision-id", g_strdup(id),
                           g_free);
    g_object_set_data(G_OBJECT(button), "entry", entry);
    return button;
}

/*
 * A proposal's review, in place of a question's options and answer box.
 *
 * Everything the agent wrote arrives escaped from the daemon and is
 * drawn as plain text -- the reason as a paragraph, the diff or the
 * whole SKILL.md in monospace -- never as markdown, which would render
 * the agent's text as if it were the page's own.
 */
static void
lrg_proposal_review(LrgWidget *body, const gchar *id, JsonObject *proposal)
{
    const gchar *reason = clawt_ipc_payload_string(proposal, "reason");
    g_auto(GStrv) warnings = clawt_ipc_payload_strv(proposal, "warnings");
    LrgWidget *note;
    LrgWidget *buttons = clawt_lrg_hbox(8.0f);
    gboolean update = g_strcmp0(clawt_ipc_payload_string(proposal, "kind"),
                                "update") == 0;
    guint i;

    if (reason != NULL && *reason != '\0') {
        g_autofree gchar *why = g_strdup_printf("Why: %s", reason);

        clawt_lrg_take(body, clawt_lrg_paragraph(why, NULL));
    }

    for (i = 0; warnings != NULL && warnings[i] != NULL; i++) {
        LrgWidget *line = clawt_lrg_hbox(10.0f);
        LrgWidget *text = clawt_lrg_paragraph(warnings[i], NULL);

        lrg_widget_set_fill_weight(text, 1.0f);
        clawt_lrg_take(line, clawt_lrg_badge("Warning", "warn"));
        clawt_lrg_take(line, text);
        clawt_lrg_take(body, line);
    }

    if (clawt_ipc_payload_boolean(proposal, "stale", FALSE))
        clawt_lrg_take(body, clawt_lrg_paragraph(
                                 "The file on disk changed after this was "
                                 "proposed, so Approve is refused. Edit "
                                 "shows what is there now, or reject it.",
                                 "muted"));

    clawt_lrg_take(body, clawt_lrg_label(update ? "The change"
                                                : "The proposed SKILL.md",
                                         "caption"));
    clawt_lrg_take(body, clawt_lrg_paragraph(clawt_lrg_proposal_shown(proposal),
                                             "mono"));

    note = clawt_lrg_entry(NULL, "Why (optional, sent to the agent)");
    lrg_widget_set_fill_weight(note, 1.0f);
    clawt_lrg_take(body, note);

    clawt_lrg_take(buttons, proposal_button(
        "Approve", "primary", id, G_CALLBACK(on_proposal_action_clicked),
        (gpointer)"approve", note));
    clawt_lrg_take(buttons, proposal_button(
        "Edit", NULL, id, G_CALLBACK(on_proposal_edit_clicked), NULL, NULL));
    clawt_lrg_take(buttons, proposal_button(
        "Reject", "danger", id, G_CALLBACK(on_proposal_action_clicked),
        (gpointer)"reject", note));
    lrg_widget_set_halign(buttons, LRG_ALIGN_START);
    clawt_lrg_take(body, buttons);
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
decision_option_button(const gchar *id, const gchar *option, LrgWidget *entry,
                       gboolean destructive)
{
    LrgWidget *row = clawt_lrg_hbox(10.0f);
    LrgWidget *text = clawt_lrg_paragraph(option, NULL);
    LrgWidget *pick = clawt_lrg_button("Choose", destructive ? "danger" : NULL,
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
    g_autofree gchar *headline = NULL;
    JsonObject *proposal = NULL;
    LrgWidget *body = clawt_lrg_vbox(8.0f);
    LrgWidget *head = clawt_lrg_hbox(8.0f);
    LrgWidget *badges = clawt_lrg_hbox(6.0f);
    LrgWidget *entry = NULL;
    LrgWidget *send;
    /*
     * A rebuild request: a yes or no about destroying a machine, with
     * what goes said on its own and no typed answer -- only "approve"
     * rebuilds, and anything typed would read as a yes that is not one.
     */
    gboolean is_rebuild = g_strcmp0(clawt_ipc_payload_string(decision, "kind"),
                                    "rebuild") == 0;
    const gchar *loses = clawt_ipc_payload_string(decision, "loses");
    LrgWidget *drop;
    LrgWidget *remove;
    LrgWidget *closing = clawt_lrg_hbox(8.0f);
    LrgWidget *card;
    guint i;

    if (id == NULL)
        id = "";

    /*
     * A skill proposal, by the library's rule.  Its headline is the only
     * line the daemon built from nothing the agent chose freely, so it
     * alone is the heading; the fenced rest is drawn by the review.
     */
    if (clawt_skill_proposal_id_is_valid(id))
        proposal = clawt_lrg_proposal_find(decisions.proposals, id);

    if (proposal != NULL) {
        headline = clawt_lrg_proposal_title(decision);
        question = headline;
    }

    /* who asked and when, with the urgency beside it */
    clawt_lrg_take(head, clawt_lrg_label(meta, "caption"));
    lrg_widget_set_fill_weight(head, 1.0f);

    if (clawt_ipc_payload_boolean(decision, "urgent", FALSE))
        clawt_lrg_take(badges, clawt_lrg_badge("closing soon", "warn"));

    if (clawt_ipc_payload_boolean(decision, "settled_by_default", FALSE))
        clawt_lrg_take(badges, clawt_lrg_badge("already decided", "bad"));

    if (is_rebuild)
        clawt_lrg_take(badges, clawt_lrg_badge("rebuild", "bad"));

    {
        LrgWidget *top = clawt_lrg_hbox(8.0f);

        clawt_lrg_take(top, head);
        clawt_lrg_take(top, badges);
        clawt_lrg_take(body, top);
    }

    clawt_lrg_take(body, clawt_lrg_paragraph((question != NULL) ? question
                                                                : "",
                                             "heading"));

    if (is_rebuild && loses != NULL) {
        g_autofree gchar *said = g_strdup_printf("What a rebuild destroys: %s",
                                                 loses);
        LrgWidget *warning = clawt_lrg_vbox(4.0f);

        clawt_lrg_take(warning, clawt_lrg_badge("will be lost", "warn"));
        clawt_lrg_take(warning, clawt_lrg_paragraph(said, NULL));
        clawt_lrg_take(body, warning);
    }

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

    if (proposal != NULL) {
        lrg_proposal_review(body, id, proposal);
        goto closing_row;
    }

    /*
     * The answer box and its button on one line, because they are one
     * action -- and made first, so the option buttons can read it: a
     * typed answer wins over a clicked option.
     */
    if (!is_rebuild) {
        entry = clawt_lrg_entry(NULL, "\"neither, do X\" is a perfectly good "
                                      "answer");
        lrg_widget_set_fill_weight(entry, 1.0f);
    }

    for (i = 0; options[i] != NULL; i++)
        clawt_lrg_take(body, decision_option_button(id, options[i], entry,
                                                    is_rebuild &&
                                                    g_strcmp0(options[i],
                                                              "approve") == 0));

    if (entry != NULL) {
        LrgWidget *typed = clawt_lrg_hbox(8.0f);

        send = clawt_lrg_button("Answer", "primary",
                                G_CALLBACK(on_answer_clicked), NULL);
        g_object_set_data_full(G_OBJECT(send), "decision-id", g_strdup(id),
                               g_free);
        g_object_set_data(G_OBJECT(send), "entry", entry);
        g_signal_connect(entry, "submitted", G_CALLBACK(on_answer_submitted),
                         send);
        clawt_lrg_take(typed, entry);
        clawt_lrg_take(typed, send);
        clawt_lrg_take(body, typed);
    }

closing_row:
    /* a proposal's review replaces the answer row it never drew into */
    if (proposal != NULL)
        g_object_unref(typed);

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

/*
 * The proposals first, then the decisions, so every proposal card is
 * drawn knowing its proposal.  A failure here -- skills turned off -- is
 * not a failure of the inbox: the decisions still come, and a proposal's
 * card falls back to the plain question, which the daemon wrote whole.
 */
static void
on_proposals(ClawtLrgApp *app, JsonNode *reply, const GError *error,
             gpointer user_data)
{
    if (GPOINTER_TO_UINT(user_data) != decisions.seq ||
        decisions.list == NULL)
        return;

    g_clear_pointer(&decisions.proposals, json_node_unref);
    if (reply != NULL)
        decisions.proposals = json_node_ref(reply);

    clawt_lrg_request(app, "decision.list", NULL, decisions.flags,
                      on_decisions, user_data, NULL);
}

static void
refresh_list(ClawtLrgApp *app, ClawtLrgRequestFlags flags)
{
    if (decisions.list == NULL)
        return;

    decisions.seq++;
    decisions.flags = flags;
    clawt_lrg_request(app, "skill.proposals", NULL, CLAWT_LRG_QUIET,
                      on_proposals, GUINT_TO_POINTER(decisions.seq), NULL);
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
