/*
 * lrg-grok.c - The Grok page: the bots, sending them work, and what came back
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Mirrors clients/gtk/gtk-grok.c: whether a job could be sent right now,
 * the bots with who may reach each and their routine prompt and MCP key,
 * a composer, and the jobs with libclawt's tone on each state.  Requests
 * go through clawt_lrg_request() only, and the page re-reads on the
 * shell's coalesced refresh rather than once per event -- a busy fleet
 * reporting progress every few seconds must not cost a request a frame.
 *
 * A report is shown in the copyable text dialog, never as markdown: it
 * was written by a model outside the fleet, and nothing it says should be
 * able to become a link here.
 */

#include "clawt-lrg.h"
#include "clawt-lrg-settings-model.h"

/* Rows drawn; the rest are counted, so a long history costs what it shows. */
#define JOBS_SHOWN (30)

static struct {
    ClawtLrgApp *app;
    LrgWidget   *page;
    LrgWidget   *status;
    LrgWidget   *bots;
    LrgWidget   *pickers;      /* the bot and agent combos, rebuilt */
    LrgWidget   *bot_combo;    /* weak: owned by pickers */
    LrgWidget   *agent_combo;  /* weak: owned by pickers */
    LrgWidget   *instructions;
    LrgWidget   *context;
    LrgWidget   *jobs;
    guint        generation;
    gboolean     in_flight;
    gboolean     again;
} grok;

static void request_grok(ClawtLrgApp *app);

/* ── Actions ──────────────────────────────────────────────────────── */

static void
on_prompt_read(ClawtLrgApp *app, JsonNode *reply, const GError *error,
               gpointer user_data)
{
    JsonObject *root = clawt_ipc_reply_object(reply);

    (void)error;
    (void)user_data;

    if (root == NULL)
        return;

    clawt_lrg_show_text(app, "Routine prompt",
                        "Paste this into the bot's routine in Grok, once. "
                        "Every job also carries its own reporting "
                        "instructions.",
                        clawt_ipc_payload_string(root, "text"));
}

static void
on_prompt_clicked(LrgButton *button, gpointer user_data)
{
    const gchar *bot = g_object_get_data(G_OBJECT(button), "bot");
    g_autoptr(JsonBuilder) builder = json_builder_new();

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "bot");
    json_builder_add_string_value(builder, bot);
    json_builder_end_object(builder);

    clawt_lrg_request(user_data, "grok.routine_prompt",
                      json_builder_get_root(builder), CLAWT_LRG_REQUEST_NONE,
                      on_prompt_read, NULL, NULL);
}

static void
on_key_minted(ClawtLrgApp *app, JsonNode *reply, const GError *error,
              gpointer user_data)
{
    JsonObject *root = clawt_ipc_reply_object(reply);
    g_autofree gchar *text = NULL;
    gboolean once;

    (void)error;
    (void)user_data;

    if (root == NULL)
        return;

    text = g_strdup_printf("URL:     %s\nAPI key: %s\n",
                           clawt_ipc_payload_string(root, "mcp_url") != NULL
                               ? clawt_ipc_payload_string(root, "mcp_url")
                               : "(not reachable yet)",
                           clawt_ipc_payload_string(root, "key"));

    /* Said as shown-once because the daemon said so. */
    once = clawt_ipc_payload_boolean(root, "secret_shown_once", FALSE);

    clawt_lrg_show_text(app, "Copy this now",
                        once ? "In Grok, ask the bot to add a remote MCP "
                               "connection with this URL and API key. This "
                               "is the only time the key is shown; minting "
                               "another replaces it."
                             : "In Grok, ask the bot to add a remote MCP "
                               "connection with this URL and API key.",
                        text);
    request_grok(app);
}

static void
on_mint_confirmed(ClawtLrgApp *app, gpointer user_data)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "bot");
    json_builder_add_string_value(builder, user_data);
    json_builder_end_object(builder);

    clawt_lrg_request(app, "grok.mcp_key", json_builder_get_root(builder),
                      CLAWT_LRG_REQUEST_NONE, on_key_minted, NULL, NULL);
}

static void
on_mint_clicked(LrgButton *button, gpointer user_data)
{
    const gchar *bot = g_object_get_data(G_OBJECT(button), "bot");
    g_autofree gchar *title = g_strdup_printf("Mint an MCP key for %s?", bot);

    clawt_lrg_confirm(user_data, title,
                      "The key is shown once. If the bot already has one, "
                      "that one stops working at once and the bot has to be "
                      "given the new one.",
                      "Mint", FALSE, on_mint_confirmed, g_strdup(bot),
                      g_free);
}

static void
on_sent(ClawtLrgApp *app, JsonNode *reply, const GError *error,
        gpointer user_data)
{
    JsonObject *root = clawt_ipc_reply_object(reply);
    JsonObject *job;
    g_autofree gchar *said = NULL;

    (void)user_data;

    if (root == NULL || error != NULL)
        return;

    job = json_object_has_member(root, "job") &&
              JSON_NODE_HOLDS_OBJECT(json_object_get_member(root, "job"))
          ? json_object_get_object_member(root, "job") : NULL;

    said = g_strdup_printf("Sent to %s as %s",
                           clawt_ipc_payload_string(job, "bot"),
                           clawt_ipc_payload_string(job, "id"));
    clawt_lrg_toast(app, said);

    lrg_text_input_set_text(LRG_TEXT_INPUT(grok.context), "");
    request_grok(app);
}

static void
on_send_clicked(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autofree gchar *instructions = clawt_lrg_editor_text(grok.instructions);
    const gchar *bot = grok.bot_combo != NULL
                       ? clawt_lrg_combo_id(grok.bot_combo) : NULL;
    const gchar *agent = grok.agent_combo != NULL
                         ? clawt_lrg_combo_id(grok.agent_combo) : NULL;
    const gchar *context = clawt_lrg_entry_text(grok.context);

    (void)button;

    if (bot == NULL || agent == NULL) {
        clawt_lrg_toast(app, "Choose a bot and an agent first");
        return;
    }

    if (instructions == NULL || *g_strstrip(instructions) == '\0') {
        clawt_lrg_toast(app, "Say what the bot should do");
        return;
    }

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "bot");
    json_builder_add_string_value(builder, bot);
    json_builder_set_member_name(builder, "agent");
    json_builder_add_string_value(builder, agent);
    json_builder_set_member_name(builder, "instructions");
    json_builder_add_string_value(builder, instructions);

    if (context != NULL && *context != '\0') {
        json_builder_set_member_name(builder, "context");
        json_builder_add_string_value(builder, context);
    }

    json_builder_end_object(builder);

    clawt_lrg_request(app, "grok.send", json_builder_get_root(builder),
                      CLAWT_LRG_REQUEST_NONE, on_sent, NULL, NULL);
}

static void
on_report_clicked(LrgButton *button, gpointer user_data)
{
    const gchar *title = g_object_get_data(G_OBJECT(button), "title");
    const gchar *text = g_object_get_data(G_OBJECT(button), "text");

    clawt_lrg_show_text(user_data, title,
                        "Written by the Grok bot, outside the fleet.", text);
}

/* ── Drawing ──────────────────────────────────────────────────────── */

static LrgWidget *
bot_row(ClawtLrgApp *app, JsonObject *bot)
{
    const gchar *name = clawt_ipc_payload_string(bot, "name");
    const gchar *description = clawt_ipc_payload_string(bot, "description");
    JsonArray *agents = clawt_lrg_settings_member_array(bot, "agents");
    g_autoptr(GString) subtitle = g_string_new(
        description != NULL ? description : "");
    LrgWidget *row;
    LrgWidget *prompt;
    LrgWidget *mint;
    guint a;

    if (subtitle->len > 0)
        g_string_append(subtitle, " \xc2\xb7 ");

    g_string_append(subtitle, "reachable by ");

    for (a = 0; agents != NULL && a < json_array_get_length(agents); a++) {
        if (a > 0)
            g_string_append(subtitle, ", ");
        g_string_append(subtitle, json_array_get_string_element(agents, a));
    }

    row = clawt_lrg_row(name != NULL ? name : "?", subtitle->str);

    if (!clawt_ipc_payload_boolean(bot, "key_configured", FALSE))
        clawt_lrg_row_add_suffix(row, clawt_lrg_badge("no key", "bad"));

    clawt_lrg_row_add_suffix(row, clawt_lrg_badge(
        clawt_ipc_payload_boolean(bot, "mcp_key", FALSE) ? "MCP"
                                                         : "HTTP only",
        clawt_ipc_payload_boolean(bot, "mcp_key", FALSE) ? "good"
                                                         : "neutral"));

    prompt = clawt_lrg_button("Routine prompt", "flat",
                              G_CALLBACK(on_prompt_clicked), app);
    g_object_set_data_full(G_OBJECT(prompt), "bot", g_strdup(name), g_free);
    clawt_lrg_row_add_suffix(row, prompt);

    mint = clawt_lrg_button("Mint MCP key", "flat",
                            G_CALLBACK(on_mint_clicked), app);
    g_object_set_data_full(G_OBJECT(mint), "bot", g_strdup(name), g_free);
    clawt_lrg_row_add_suffix(row, mint);

    return row;
}

static LrgWidget *
job_row(ClawtLrgApp *app, JsonObject *job)
{
    gint state = CLAWT_GROK_JOB_FAILED;
    const gchar *result = clawt_ipc_payload_string(job, "result");
    const gchar *note = clawt_ipc_payload_string(job, "note");
    const gchar *body = result != NULL && *result != '\0' ? result : note;
    g_autofree gchar *age = clawt_lrg_ago(
        clawt_ipc_payload_int(job, "created_at", 0));
    g_autofree gchar *title = g_strdup_printf(
        "%s \xc2\xb7 %s", clawt_ipc_payload_string(job, "bot"),
        clawt_ipc_payload_string(job, "instructions") != NULL
            ? clawt_ipc_payload_string(job, "instructions") : "");
    g_autofree gchar *subtitle = g_strdup_printf(
        "%s for %s, %s", clawt_ipc_payload_string(job, "id"),
        clawt_ipc_payload_string(job, "agent"), age);
    LrgWidget *row;

    /* The nick resolved to the enum, never compared as a spelling here. */
    clawt_enum_from_nick(CLAWT_TYPE_GROK_JOB_STATE,
                         clawt_ipc_payload_string(job, "state"), &state);

    row = clawt_lrg_row(title, subtitle);
    clawt_lrg_row_add_prefix(row, clawt_lrg_badge(
        clawt_grok_job_state_label((ClawtGrokJobState)state),
        clawt_grok_job_state_tone((ClawtGrokJobState)state)));

    if (body != NULL && *body != '\0') {
        LrgWidget *show = clawt_lrg_button(
            state == CLAWT_GROK_JOB_REFUSED ? "Why" : "Report", "flat",
            G_CALLBACK(on_report_clicked), app);

        g_object_set_data_full(G_OBJECT(show), "title",
                               g_strdup(clawt_ipc_payload_string(job, "id")),
                               g_free);
        g_object_set_data_full(G_OBJECT(show), "text", g_strdup(body),
                               g_free);
        clawt_lrg_row_add_suffix(row, show);
    }

    return row;
}

static void
rebuild_pickers(JsonArray *bots)
{
    g_autoptr(GPtrArray) bot_ids = g_ptr_array_new();
    g_autoptr(GPtrArray) agent_ids = g_ptr_array_new();
    g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
    g_autofree gchar *keep_bot = g_strdup(
        grok.bot_combo != NULL ? clawt_lrg_combo_id(grok.bot_combo) : NULL);
    g_autofree gchar *keep_agent = g_strdup(
        grok.agent_combo != NULL ? clawt_lrg_combo_id(grok.agent_combo)
                                 : NULL);
    LrgWidget *form = clawt_lrg_form_new();
    guint i;

    for (i = 0; bots != NULL && i < json_array_get_length(bots); i++) {
        JsonObject *bot = json_array_get_object_element(bots, i);
        JsonArray *agents = clawt_lrg_settings_member_array(bot, "agents");
        guint a;

        g_ptr_array_add(bot_ids,
                        (gpointer)clawt_ipc_payload_string(bot, "name"));

        for (a = 0; agents != NULL && a < json_array_get_length(agents);
             a++) {
            const gchar *id = json_array_get_string_element(agents, a);

            if (g_hash_table_contains(seen, id))
                continue;

            g_hash_table_add(seen, (gpointer)id);
            g_ptr_array_add(agent_ids, (gpointer)id);
        }
    }

    g_ptr_array_add(bot_ids, NULL);
    g_ptr_array_add(agent_ids, NULL);

    clawt_lrg_clear(grok.pickers);

    /* The choice somebody made survives a refresh, by id. */
    grok.bot_combo = clawt_lrg_combo((const gchar *const *)bot_ids->pdata,
                                     (const gchar *const *)bot_ids->pdata,
                                     keep_bot);
    clawt_lrg_form_add(form, "Bot", grok.bot_combo, NULL);

    grok.agent_combo = clawt_lrg_combo((const gchar *const *)agent_ids->pdata,
                                       (const gchar *const *)agent_ids->pdata,
                                       keep_agent);
    clawt_lrg_form_add(form, "On behalf of", grok.agent_combo,
                       "The agent the answer is delivered to.");

    clawt_lrg_take(grok.pickers, form);
}

static void
on_jobs(ClawtLrgApp *app, JsonNode *reply, const GError *error,
        gpointer user_data)
{
    JsonObject *root = clawt_ipc_reply_object(reply);
    JsonArray *jobs;
    guint i;
    guint n;

    (void)error;

    grok.in_flight = FALSE;

    if (GPOINTER_TO_UINT(user_data) == grok.generation && grok.jobs != NULL) {
        clawt_lrg_clear(grok.jobs);

        jobs = root != NULL ? clawt_lrg_settings_member_array(root, "jobs")
                            : NULL;
        n = jobs != NULL ? json_array_get_length(jobs) : 0;

        if (root == NULL)
            clawt_lrg_take(grok.jobs, clawt_lrg_empty_state(
                "The Grok jobs are unavailable",
                "Check the daemon connection, then reopen Grok to retry."));
        else if (n == 0)
            clawt_lrg_take(grok.jobs, clawt_lrg_empty_state(
                "No work has been handed to a Grok bot",
                "Agents send it with clawtilla_grok_dispatch; you can send "
                "it from here."));

        for (i = 0; i < n && i < JOBS_SHOWN; i++) {
            LrgWidget *row = job_row(app,
                                     json_array_get_object_element(jobs, i));

            clawt_lrg_appear(app, row, i);
            clawt_lrg_take(grok.jobs, row);
        }

        if (n > JOBS_SHOWN) {
            g_autofree gchar *more = g_strdup_printf(
                "%u older jobs not shown; `clawtilla grok jobs` lists them.",
                n - JOBS_SHOWN);

            clawt_lrg_take(grok.jobs, clawt_lrg_label(more, "muted"));
        }

        if (root != NULL)
            clawt_lrg_set_page_badge(
                app, CLAWT_PAGE_GROK,
                (guint)clawt_ipc_payload_int(root, "attention", 0), "bad");

        clawt_lrg_app_wake(app);
    }

    if (grok.again) {
        grok.again = FALSE;
        request_grok(app);
    }
}

static void
on_bots(ClawtLrgApp *app, JsonNode *reply, const GError *error,
        gpointer user_data)
{
    JsonObject *root = clawt_ipc_reply_object(reply);
    JsonArray *bots;
    guint i;

    (void)error;

    if (GPOINTER_TO_UINT(user_data) != grok.generation || grok.bots == NULL ||
        root == NULL) {
        grok.in_flight = FALSE;
        return;
    }

    if (clawt_ipc_payload_boolean(root, "reachable", FALSE)) {
        g_autofree gchar *where = g_strdup_printf(
            "Bots report back to %s over the tailnet.",
            clawt_ipc_payload_string(root, "callback_host"));

        clawt_lrg_paragraph_set_text(grok.status, where);
    } else {
        g_autofree gchar *why = g_strdup_printf(
            "No job can be sent right now: %s.",
            clawt_ipc_payload_string(root, "unreachable_reason"));

        clawt_lrg_paragraph_set_text(grok.status, why);
    }

    clawt_lrg_clear(grok.bots);
    bots = clawt_lrg_settings_member_array(root, "bots");

    if (bots == NULL || json_array_get_length(bots) == 0)
        clawt_lrg_take(grok.bots, clawt_lrg_empty_state(
            "No Grok bot is shared with any agent",
            "Add a grokbot integration in Settings, named for the bot, with "
            "its routine's webhook address and key."));

    for (i = 0; bots != NULL && i < json_array_get_length(bots); i++)
        clawt_lrg_take(grok.bots,
                       bot_row(app, json_array_get_object_element(bots, i)));

    rebuild_pickers(bots);

    clawt_lrg_request(app, "grok.jobs", NULL, CLAWT_LRG_QUIET, on_jobs,
                      user_data, NULL);
}

static void
request_grok(ClawtLrgApp *app)
{
    if (grok.jobs == NULL)
        return;

    /* One in flight; events meanwhile ask for one more read, not one each. */
    if (grok.in_flight) {
        grok.again = TRUE;
        return;
    }

    grok.in_flight = TRUE;
    grok.generation++;
    clawt_lrg_request(app, "grok.bots", NULL, CLAWT_LRG_QUIET, on_bots,
                      GUINT_TO_POINTER(grok.generation), NULL);
}

static void
on_refresh_clicked(LrgButton *button, gpointer user_data)
{
    (void)button;
    request_grok(user_data);
}

/* ── The page ─────────────────────────────────────────────────────── */

static LrgWidget *
build(ClawtLrgApp *app)
{
    LrgWidget *body;
    LrgWidget *composer = clawt_lrg_vbox(8.0f);
    LrgWidget *form = clawt_lrg_form_new();

    grok.app = app;
    grok.page = clawt_lrg_page_new("Grok",
                                   "Grok bots the fleet hands work to");
    body = clawt_lrg_page_body(grok.page);

    grok.status = clawt_lrg_paragraph("", "muted");
    clawt_lrg_add(body, grok.status);
    g_object_unref(grok.status);

    clawt_lrg_take(body, clawt_lrg_section("Bots"));
    grok.bots = clawt_lrg_vbox(6.0f);
    clawt_lrg_add(body, grok.bots);
    g_object_unref(grok.bots);

    clawt_lrg_take(body, clawt_lrg_section("Hand a bot some work"));

    grok.pickers = clawt_lrg_vbox(0.0f);
    clawt_lrg_add(composer, grok.pickers);
    g_object_unref(grok.pickers);

    grok.instructions = clawt_lrg_editor(NULL, NULL);
    lrg_widget_set_min_size(grok.instructions, 0.0f, 120.0f);
    clawt_lrg_form_add(form, "What the bot should do", grok.instructions,
                       "It knows nothing of this fleet: say everything.");

    grok.context = clawt_lrg_entry(NULL, "Optional; sent as data");
    clawt_lrg_form_add(form, "Context", grok.context, NULL);
    clawt_lrg_take(composer, form);

    clawt_lrg_take(composer,
                   clawt_lrg_button("Send to the bot", "primary",
                                    G_CALLBACK(on_send_clicked), app));
    clawt_lrg_take(body, clawt_lrg_card(composer));

    clawt_lrg_take(body, clawt_lrg_section("Jobs"));
    grok.jobs = clawt_lrg_vbox(6.0f);
    clawt_lrg_add(body, grok.jobs);
    g_object_unref(grok.jobs);

    clawt_lrg_take(clawt_lrg_page_toolbar(grok.page),
                   clawt_lrg_button("Refresh", "flat",
                                    G_CALLBACK(on_refresh_clicked), app));

    return grok.page;
}

static void
refresh(ClawtLrgApp *app)
{
    request_grok(app);
}

static void
event(ClawtLrgApp *app, ClawtEvent *event)
{
    const gchar *kind = clawt_event_get_kind(event);

    if (grok.page == NULL || app->page != CLAWT_PAGE_GROK)
        return;

    /*
     * Coalesced by request_grok(): a burst of progress notes is one read
     * in flight and one more after it, never a read per note.
     */
    if (kind != NULL && g_str_has_prefix(kind, "grok."))
        request_grok(app);
}

static const ClawtLrgPage page_impl = {
    CLAWT_PAGE_GROK, build, refresh, event
};

const ClawtLrgPage *
clawt_lrg_grok_page(void)
{
    return &page_impl;
}
