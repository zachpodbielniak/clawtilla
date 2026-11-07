/*
 * lrg-recall.c - The Memory page: fleet-wide recall and the operator model
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Mirrors clients/gtk/gtk-recall.c, and adds the selected agent's own
 * memory store, which GTK reaches only through `/memory` in the composer
 * and the web client draws on the agent page.
 *
 * Three questions, kept apart because their answers live in different
 * databases.  Recall searches what was actually said, across every
 * conversation and every session (memory.recall).  An agent's memories
 * are what it chose to keep, with the scope each came from
 * (memory.list / memory.search).  The operator profile is what the fleet
 * believes about the person running it -- here because a model of
 * somebody they cannot read is not something to build: they can read
 * every line, edit the half they wrote, and see the agent and the day
 * behind the half they did not (operator.get / operator.set).
 */

#include "clawt-lrg.h"
#include "clawt-lrg-fleet-model.h"

static struct {
    LrgWidget *page;

    /* recall */
    LrgWidget *recall_entry;
    LrgWidget *recall_notice;
    LrgWidget *recall_list;

    /* the selected agent's store */
    LrgWidget *memory_heading;
    LrgWidget *memory_entry;
    LrgWidget *memory_list;
    JsonNode  *memory_reply;

    /* the operator profile */
    LrgWidget *operator_banner;
    LrgWidget *operator_banner_label;
    LrgWidget *operator_view;
    LrgWidget *operator_path;
    LrgWidget *operator_learned;
    gchar     *operator_loaded;   /* the text as last read */
} state;

/*
 * A row that says why a list is empty, rather than an empty list.
 * "Nothing matched" and "nothing has ever been said" are different
 * situations with different next steps, and a bare empty box says
 * neither.
 */
static LrgWidget *
placeholder(const gchar *title, const gchar *body)
{
    LrgWidget *row = clawt_lrg_vbox(2.0f);

    lrg_widget_add_class(row, "clawt-row");
    lrg_container_set_padding(LRG_CONTAINER(row), 12.0f);
    clawt_lrg_take(row, clawt_lrg_label(title, NULL));
    clawt_lrg_take(row, clawt_lrg_paragraph(body, "muted"));

    return row;
}

/* A row of a body that wraps, a line under it, and a suffix. */
static LrgWidget *
text_row(const gchar *body, const gchar *subtitle, LrgWidget *suffix)
{
    LrgWidget *row = clawt_lrg_hbox(12.0f);
    LrgWidget *text = clawt_lrg_vbox(4.0f);

    lrg_widget_add_class(row, "clawt-row");
    lrg_container_set_padding(LRG_CONTAINER(row), 12.0f);
    lrg_widget_set_fill_weight(text, 1.0f);

    /* plain, never markdown: a hit is somebody's words, quoted */
    clawt_lrg_take(text, clawt_lrg_paragraph(body, NULL));

    if (subtitle != NULL && *subtitle != '\0')
        clawt_lrg_take(text, clawt_lrg_label(subtitle, "muted"));

    clawt_lrg_take(row, text);

    if (suffix != NULL) {
        lrg_widget_set_valign(suffix, LRG_ALIGN_START);
        clawt_lrg_take(row, suffix);
    }

    return row;
}

/* ── Recall ───────────────────────────────────────────────────────── */

static void
show_recall_prompt(void)
{
    clawt_lrg_clear(state.recall_list);
    lrg_widget_set_visible(state.recall_notice, FALSE);
    clawt_lrg_take(state.recall_list, placeholder(
        "Search every conversation",
        "What was said, across all sessions and every room -- not what an "
        "agent chose to remember."));
}

static void
on_recalled(ClawtLrgApp *app, JsonNode *reply, const GError *error,
            gpointer user_data)
{
    g_autoptr(GPtrArray) hits = NULL;
    guint i;

    clawt_lrg_clear(state.recall_list);

    if (reply == NULL) {
        clawt_lrg_take(state.recall_list, placeholder(
            "Recall did not answer",
            error != NULL ? error->message : "The request failed."));
        clawt_lrg_app_wake(app);
        return;
    }

    /*
     * Said, not inferred: a thin result from a sqlite without FTS5 looks
     * exactly like a thin result from a ranked search.
     */
    lrg_widget_set_visible(state.recall_notice,
                           clawt_lrg_recall_is_substring(reply));

    hits = clawt_lrg_recall_hits(reply);

    if (hits->len == 0) {
        const gchar *title;
        const gchar *body;

        clawt_lrg_recall_empty(reply, &title, &body);
        clawt_lrg_take(state.recall_list, placeholder(title, body));
    }

    for (i = 0; i < hits->len; i++) {
        ClawtLrgRecallHit *hit = g_ptr_array_index(hits, i);
        LrgWidget *row = text_row(
            hit->body, hit->subtitle,
            hit->stamp != NULL ? clawt_lrg_label(hit->stamp, "caption")
                               : NULL);

        lrg_widget_add_class(row, "recall-hit");
        clawt_lrg_appear(app, row, i);
        clawt_lrg_take(state.recall_list, row);
    }

    clawt_lrg_app_wake(app);
}

static void
recall(ClawtLrgApp *app)
{
    JsonNode *payload = clawt_lrg_recall_payload(
        clawt_lrg_entry_text(state.recall_entry));

    /* the daemon refuses an empty query; say what the box is for */
    if (payload == NULL) {
        show_recall_prompt();
        return;
    }

    clawt_lrg_request(app, "memory.recall", payload, CLAWT_LRG_REQUEST_NONE,
                      on_recalled, NULL, NULL);
}

void
clawt_lrg_recall_search(ClawtLrgApp *app, const gchar *query)
{
    /* the page is built on first show, which /recall has just done */
    if (state.recall_entry == NULL)
        return;

    lrg_text_input_set_text(LRG_TEXT_INPUT(state.recall_entry),
                            (query != NULL) ? query : "");
    recall(app);
}

static void
on_recall_submitted(LrgTextInput *entry, gpointer user_data)
{
    recall(user_data);
}

static void
on_recall_clicked(LrgButton *button, gpointer user_data)
{
    recall(user_data);
}

/* ── The selected agent's memories ────────────────────────────────── */

static void
draw_memories(ClawtLrgApp *app)
{
    g_autoptr(GPtrArray) rows = NULL;
    guint i;

    clawt_lrg_clear(state.memory_list);

    /*
     * Every scope, unfiltered.  A scope filter would walk
     * clawt_memory_scope_*(), and a choice enumeration offered by one
     * client must be offered by all three (make parity, layer 3); GTK
     * and the web client offer none here, so neither does this.  The
     * scope is still drawn on each row.
     */
    rows = clawt_lrg_agent_memories(state.memory_reply, NULL,
                                    g_get_real_time());

    if (rows->len == 0) {
        clawt_lrg_take(state.memory_list, placeholder(
            "Nothing remembered here",
            "An agent writes its memories through its own tools; a search "
            "can also narrow this to nothing."));
        return;
    }

    for (i = 0; i < rows->len; i++) {
        ClawtLrgMemoryRow *row = g_ptr_array_index(rows, i);
        LrgWidget *card = clawt_lrg_vbox(6.0f);
        LrgWidget *head = clawt_lrg_hbox(8.0f);
        LrgWidget *title = clawt_lrg_label(row->title, NULL);

        lrg_widget_add_class(card, "clawt-row");
        lrg_container_set_padding(LRG_CONTAINER(card), 12.0f);
        lrg_widget_set_fill_weight(title, 1.0f);
        clawt_lrg_take(head, title);

        if (row->scope_label != NULL)
            clawt_lrg_take(head, clawt_lrg_badge(row->scope_label, "info"));

        if (row->category != NULL)
            clawt_lrg_take(head, clawt_lrg_badge(row->category, "neutral"));

        if (row->pinned)
            clawt_lrg_take(head, clawt_lrg_badge("pinned", "warn"));

        if (*row->when != '\0')
            clawt_lrg_take(head, clawt_lrg_label(row->when, "caption"));

        clawt_lrg_take(card, head);

        /* the whole content only when the title was cut from it */
        if (g_strcmp0(row->title, row->body) != 0)
            clawt_lrg_take(card, clawt_lrg_paragraph(row->body, "muted"));

        clawt_lrg_appear(app, card, i);
        clawt_lrg_take(state.memory_list, card);
    }
}

static void
on_memories(ClawtLrgApp *app, JsonNode *reply, const GError *error,
            gpointer user_data)
{
    g_clear_pointer(&state.memory_reply, json_node_unref);

    if (reply == NULL) {
        /*
         * Quiet, and said here instead: "memories.enabled is off" is the
         * ordinary answer for most agents, and a toast every time the
         * page is shown would bury the window.
         */
        clawt_lrg_clear(state.memory_list);
        clawt_lrg_take(state.memory_list, placeholder(
            "No memories to show",
            error != NULL ? error->message : "The request failed."));
        clawt_lrg_app_wake(app);
        return;
    }

    state.memory_reply = json_node_copy(reply);
    draw_memories(app);
    clawt_lrg_app_wake(app);
}

static void
load_memories(ClawtLrgApp *app)
{
    JsonNode *payload;
    gboolean search = FALSE;
    g_autofree gchar *heading = NULL;

    clawt_lrg_clear(state.memory_list);
    g_clear_pointer(&state.memory_reply, json_node_unref);

    if (app->agent_id == NULL) {
        lrg_label_set_text(LRG_LABEL(state.memory_heading),
                           "An agent's memories");
        clawt_lrg_take(state.memory_list, placeholder(
            "Choose an agent",
            "Each agent keeps its own memories, and sees its team's and "
            "the fleet's beside them."));
        return;
    }

    heading = g_strdup_printf("What %s remembers", app->agent_id);
    lrg_label_set_text(LRG_LABEL(state.memory_heading), heading);

    payload = clawt_lrg_agent_memory_payload(
        app->agent_id, clawt_lrg_entry_text(state.memory_entry), &search);

    /*
     * Guarded: the reply describes the selected agent, and one that
     * lands after somebody picked another must not be drawn under the
     * other's name.
     */
    if (search)
        clawt_lrg_request(app, "memory.search", payload,
                          CLAWT_LRG_GUARDED | CLAWT_LRG_QUIET, on_memories,
                          NULL, NULL);
    else
        clawt_lrg_request(app, "memory.list", payload,
                          CLAWT_LRG_GUARDED | CLAWT_LRG_QUIET, on_memories,
                          NULL, NULL);
}

static void
on_memory_submitted(LrgTextInput *entry, gpointer user_data)
{
    load_memories(user_data);
}

static void
on_memory_clicked(LrgButton *button, gpointer user_data)
{
    load_memories(user_data);
}

/* ── The operator profile ─────────────────────────────────────────── */

static void
on_operator(ClawtLrgApp *app, JsonNode *reply, const GError *error,
            gpointer user_data)
{
    JsonObject *payload = clawt_ipc_reply_object(reply);
    g_autoptr(GPtrArray) learned = NULL;
    g_autofree gchar *current = NULL;
    const gchar *text;
    const gchar *path;
    guint i;

    clawt_lrg_clear(state.operator_learned);

    if (reply == NULL) {
        clawt_lrg_paragraph_set_text(state.operator_banner_label,
                           error != NULL ? error->message
                                         : "The operator profile did not "
                                           "answer.");
        lrg_widget_set_visible(state.operator_banner, TRUE);
        clawt_lrg_app_wake(app);
        return;
    }

    text = clawt_ipc_payload_string(payload, "text");
    path = clawt_ipc_payload_string(payload, "path");
    current = clawt_lrg_editor_text(state.operator_view);

    /*
     * The page re-reads whenever it is shown; what somebody typed and
     * has not saved is theirs, and a re-read that replaced it would lose
     * an edit to a tab switch.  Only an unchanged editor follows the
     * daemon.
     */
    if (state.operator_loaded == NULL ||
        g_strcmp0(current, state.operator_loaded) == 0)
        lrg_text_editor_set_text(LRG_TEXT_EDITOR(state.operator_view),
                                 text != NULL ? text : "");

    g_free(state.operator_loaded);
    state.operator_loaded = g_strdup(text != NULL ? text : "");

    lrg_label_set_text(LRG_LABEL(state.operator_path),
                       path != NULL ? path : "");

    /*
     * A banner, because this is a condition the page is in until
     * somebody changes a setting: text typed into a profile nobody reads
     * is worse than an empty page.
     */
    clawt_lrg_paragraph_set_text(state.operator_banner_label,
                       CLAWT_LRG_OPERATOR_OFF);
    lrg_widget_set_visible(state.operator_banner,
                           !clawt_ipc_payload_boolean(payload, "enabled",
                                                      FALSE));

    learned = clawt_lrg_operator_learned(reply);

    if (learned->len == 0)
        clawt_lrg_take(state.operator_learned, placeholder(
            "Nothing recorded yet",
            "Agents add to this by writing fleet-scope memories in the "
            "'operator' category."));

    for (i = 0; i < learned->len; i++) {
        ClawtLrgNote *note = g_ptr_array_index(learned, i);

        clawt_lrg_take(state.operator_learned,
                       text_row(note->title, note->body, NULL));
    }

    clawt_lrg_app_wake(app);
}

static void
load_operator(ClawtLrgApp *app)
{
    clawt_lrg_request(app, "operator.get", NULL, CLAWT_LRG_QUIET,
                      on_operator, NULL, NULL);
}

static void
on_operator_saved(ClawtLrgApp *app, JsonNode *reply, const GError *error,
                  gpointer user_data)
{
    g_autofree gchar *refused = NULL;

    if (reply == NULL)
        return;

    /*
     * The saved text is now what the daemon has, so the next re-read may
     * replace the editor again.
     */
    g_free(state.operator_loaded);
    state.operator_loaded = g_strdup(user_data);

    /*
     * A toast rather than a banner: this answers a question somebody is
     * holding right now -- "did that save" -- and says what it reached.
     * An agent whose files could not be re-rendered is said too.
     */
    refused = clawt_ipc_reply_refusal_text(reply, NULL);
    clawt_lrg_toast(app, refused != NULL
                             ? refused
                             : "Saved. Every agent's USER.org was rewritten.");
    load_operator(app);
}

static void
on_operator_save(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    g_autofree gchar *text = clawt_lrg_editor_text(state.operator_view);

    clawt_lrg_request(app, "operator.set",
                      clawt_ipc_payload_new("text", text, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_operator_saved,
                      g_strdup(text), g_free);
}

/* ── The page ─────────────────────────────────────────────────────── */

/* A condition the page is in, in the banner material; takes @child. */
static LrgWidget *
banner(LrgWidget *child)
{
    LrgWidget *box = clawt_lrg_hbox(8.0f);

    lrg_widget_add_class(box, "clawt-banner");
    lrg_container_set_padding(LRG_CONTAINER(box), 12.0f);
    lrg_widget_set_fill_weight(child, 1.0f);
    clawt_lrg_take(box, child);

    return box;
}

static LrgWidget *
search_line(LrgWidget *entry, const gchar *verb, GCallback clicked,
            ClawtLrgApp *app)
{
    LrgWidget *line = clawt_lrg_hbox(8.0f);

    lrg_widget_set_fill_weight(entry, 1.0f);
    clawt_lrg_add(line, entry);
    clawt_lrg_take(line, clawt_lrg_button(verb, "primary", clicked, app));

    return line;
}

static LrgWidget *
build(ClawtLrgApp *app)
{
    LrgWidget *body;
    LrgWidget *save_line;

    state.page = clawt_lrg_page_new(
        "Memory", "What the fleet said, what it kept, and what it believes "
                  "about you");
    body = clawt_lrg_page_body(state.page);

    /* ── Recall ── */
    clawt_lrg_take(body, clawt_lrg_section("Recall"));

    state.recall_entry = clawt_lrg_entry(
        NULL, "Search everything the fleet has said");
    lrg_widget_add_class(state.recall_entry, "recall-form");
    g_signal_connect(state.recall_entry, "submitted",
                     G_CALLBACK(on_recall_submitted), app);
    clawt_lrg_take(body, search_line(state.recall_entry, "Search",
                                     G_CALLBACK(on_recall_clicked), app));
    g_object_unref(state.recall_entry);

    state.recall_notice = banner(clawt_lrg_paragraph(
        "This sqlite has no FTS5, so recall is a substring match rather than "
        "a ranked search.", NULL));
    lrg_widget_set_visible(state.recall_notice, FALSE);
    clawt_lrg_add(body, state.recall_notice);
    g_object_unref(state.recall_notice);

    state.recall_list = clawt_lrg_vbox(8.0f);
    clawt_lrg_add(body, state.recall_list);
    g_object_unref(state.recall_list);
    show_recall_prompt();

    /* ── One agent's memories ── */
    state.memory_heading = clawt_lrg_section("An agent's memories");
    clawt_lrg_add(body, state.memory_heading);
    g_object_unref(state.memory_heading);

    {
        LrgWidget *line = clawt_lrg_hbox(8.0f);

        state.memory_entry = clawt_lrg_entry(NULL, "Search its memories");
        g_signal_connect(state.memory_entry, "submitted",
                         G_CALLBACK(on_memory_submitted), app);

        lrg_widget_set_fill_weight(state.memory_entry, 1.0f);
        clawt_lrg_add(line, state.memory_entry);
        clawt_lrg_take(line, clawt_lrg_button("Search", NULL,
                                              G_CALLBACK(on_memory_clicked),
                                              app));
        clawt_lrg_take(body, line);
        g_object_unref(state.memory_entry);
    }

    state.memory_list = clawt_lrg_vbox(8.0f);
    clawt_lrg_add(body, state.memory_list);
    g_object_unref(state.memory_list);

    /* ── The operator profile ── */
    /* wraps, as the window's banner does: these are sentences */
    state.operator_banner_label = clawt_lrg_paragraph("", NULL);
    state.operator_banner = banner(state.operator_banner_label);
    lrg_widget_set_visible(state.operator_banner, FALSE);
    clawt_lrg_add(body, state.operator_banner);
    g_object_unref(state.operator_banner);

    clawt_lrg_take(body, clawt_lrg_section("About you"));

    state.operator_view = clawt_lrg_editor(NULL, NULL);
    lrg_widget_add_class(state.operator_view, "operator-profile");
    lrg_widget_add_class(state.operator_view, "clawt-terminal");
    lrg_widget_set_min_size(state.operator_view, 0.0f, 160.0f);
    clawt_lrg_add(body, state.operator_view);
    g_object_unref(state.operator_view);

    save_line = clawt_lrg_hbox(8.0f);
    state.operator_path = clawt_lrg_label("", "caption");
    lrg_widget_set_fill_weight(state.operator_path, 1.0f);
    lrg_widget_set_valign(state.operator_path, LRG_ALIGN_CENTER);
    clawt_lrg_add(save_line, state.operator_path);
    g_object_unref(state.operator_path);
    clawt_lrg_take(save_line, clawt_lrg_button("Save profile", "primary",
                                               G_CALLBACK(on_operator_save),
                                               app));
    clawt_lrg_take(body, save_line);

    clawt_lrg_take(body, clawt_lrg_section("What the fleet recorded"));

    state.operator_learned = clawt_lrg_vbox(8.0f);
    clawt_lrg_add(body, state.operator_learned);
    g_object_unref(state.operator_learned);

    return state.page;
}

/*
 * A page that has to read something reads it when it is opened.  Three
 * round trips fill it, and filling it on every connect would pay for all
 * of them whether or not anybody looks.  The recall results are left as
 * they are: they answer a query somebody typed, which has not changed.
 */
static void
refresh(ClawtLrgApp *app)
{
    load_operator(app);
    load_memories(app);
}

static const ClawtLrgPage page_impl = { CLAWT_PAGE_MEMORY, build, refresh,
                                        NULL };

const ClawtLrgPage *
clawt_lrg_memory_page(void)
{
    return &page_impl;
}
