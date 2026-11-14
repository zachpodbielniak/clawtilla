/*
 * lrg-inspector.c - The Overview page: everything an agent is
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Mirrors clients/gtk/gtk-inspector.c and the choosers it borrows from
 * clawt-window.c (model, image, disk, team): what the agent is and is
 * doing, its face, every setting it can carry, its credentials (named,
 * never valued), its integrations, its shared folders, its workspace
 * files, what it has spent, and the ways to reset it or remove it.
 *
 * The settings are walked from the schema, as the web inspector's are,
 * rather than written as a row per key the way the GTK one began: an
 * option added to the schema appears here without anybody editing this
 * file (lrg-agent-model.c, clawt_lrg_agent_form()).  The keys whose
 * values the daemon knows -- team, account, shared machine, provider,
 * model, image, disk -- are drawn as choosers filled from its answers.
 *
 * A form, not LrgPropertyGrid: the grid edits GObject properties (or a
 * property source of typed values), and what is edited here is a list of
 * schema keys whose controls are filled asynchronously from five
 * different replies.  Wrapping each in a property source would be a
 * second description of the schema; the kit's form is the first.
 *
 * Every request is async (clawt_lrg_request()).  A save is a chain of
 * `agent.set` calls in the model's order, each sent when the last one
 * answered, because the order is the point: the account comes off before
 * the provider changes.
 */

#include "clawt-lrg.h"
#include "clawt-lrg-agent-model.h"

#include <string.h>

/* One setting's control, and the slot it is drawn in. */
typedef struct {
    ClawtLrgAgentField *field;      /* borrowed from ins.form */
    LrgWidget          *slot;
    LrgWidget          *control;
    LrgWidget          *other;      /* the typed-value entry, or NULL */
    LrgWidget          *note;       /* a line under a chooser, or NULL */
} FieldView;

typedef struct {
    LrgWidget  *page;
    LrgWidget  *body;
    LrgWidget  *glance;
    LrgWidget  *integrations;
    LrgWidget  *mounts;
    LrgWidget  *waits;
    LrgWidget  *files;
    LrgWidget  *usage_today;
    LrgWidget  *usage_total;
    LrgWidget  *mount_source;
    LrgWidget  *mount_target;
    LrgWidget  *mount_mode;
    LrgWidget  *mount_relabel;
    LrgWidget  *save;

    JsonNode   *shown;       /* agent.show, as the form was built from it */
    GPtrArray  *form;        /* ClawtLrgAgentGroup */
    GPtrArray  *views;       /* FieldView */
    JsonNode   *catalog;     /* model.list */
    JsonNode   *images;      /* image.list */
    JsonNode   *disks;       /* image.vm_list */
    gboolean    toggling;    /* an integration switch is being put back */

    /*
     * model.fallbacks: the editor's box, its entries' controls, the
     * account.list it offers logins from, an edit waiting to be drawn,
     * and the list as agent.show spelled it -- NULL when it could not be
     * read, which leaves the editor out of the save.
     */
    LrgWidget  *fallbacks;
    GPtrArray  *fallback_views;     /* FallbackView */
    GPtrArray  *fallback_pending;   /* ClawtModelFallback */
    JsonNode   *accounts;
    gchar      *fallbacks_saved;
} Inspector;

static Inspector ins;

static void refresh(ClawtLrgApp *app);
static void redraw_fallbacks(ClawtLrgApp *app);

/* ── Small pieces ─────────────────────────────────────────────────── */

static JsonObject *
shown_root(void)
{
    return clawt_ipc_reply_object(ins.shown);
}

static JsonObject *
shown_object(const gchar *member)
{
    JsonObject *root = shown_root();

    if (root == NULL || !json_object_has_member(root, member) ||
        !JSON_NODE_HOLDS_OBJECT(json_object_get_member(root, member)))
        return NULL;

    return json_object_get_object_member(root, member);
}

/*
 * A card with a heading and an optional sentence, added to @parent.
 *
 * Returns: (transfer none): the box inside it, for the caller to fill
 */
static LrgWidget *
section_card(ClawtLrgApp *app, LrgWidget *parent, const gchar *title,
             const gchar *summary, guint index)
{
    LrgWidget *box = clawt_lrg_vbox(10.0f);
    LrgWidget *card;

    clawt_lrg_take(box, clawt_lrg_label(title, "heading"));

    if (summary != NULL)
        clawt_lrg_take(box, clawt_lrg_paragraph(summary, "muted"));

    card = clawt_lrg_card(box);
    clawt_lrg_appear(app, card, index);
    clawt_lrg_take(parent, card);

    return box;
}

/* A label/value line, the inspector's info row. */
static LrgWidget *
info_row(const gchar *title, const gchar *value)
{
    return clawt_lrg_row(title, (value != NULL) ? value : "-");
}

/* ── At a glance ──────────────────────────────────────────────────── */

static void
on_verb(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    const gchar *kind = g_object_get_data(G_OBJECT(button), "clawt-kind");

    if (app->agent_id == NULL || kind == NULL)
        return;

    /*
     * The sidebar's helper, shared with its context menu, so both paths
     * refresh the same way -- including after a failed start, which
     * still moves the agent to ERROR.
     */
    clawt_lrg_agent_action(app, app->agent_id, kind);
}

static void
draw_glance(ClawtLrgApp *app, JsonNode *reply)
{
    JsonObject *root = clawt_ipc_reply_object(reply);
    JsonObject *agent = NULL;
    JsonObject *identity = NULL;
    ClawtLrgAgentVerb verbs[CLAWT_LRG_AGENT_VERBS];
    const gchar *state;
    LrgWidget *badges = clawt_lrg_hbox(6.0f);
    LrgWidget *actions = clawt_lrg_hbox(8.0f);
    g_autofree gchar *held = NULL;
    g_autofree gchar *identity_text = NULL;
    g_autofree gchar *fallback = NULL;
    guint i;

    clawt_lrg_clear(ins.glance);

    if (root != NULL && json_object_has_member(root, "agent"))
        agent = json_object_get_object_member(root, "agent");

    if (root != NULL && json_object_has_member(root, "identity"))
        identity = json_object_get_object_member(root, "identity");

    state = (agent != NULL) ? clawt_ipc_payload_string(agent, "state") : NULL;

    if (state == NULL)
        state = "stopped";

    clawt_lrg_take(ins.glance, clawt_lrg_label("At a glance", "heading"));

    /*
     * Held is not a state and is not drawn as one: a held agent is
     * running, and only delivery has stopped -- the opposite of stopped
     * about whether work was lost.  The library spells the badge.
     */
    clawt_lrg_take(badges, clawt_lrg_badge(state,
                                           clawt_lrg_agent_state_tone(state)));
    held = clawt_hold_label(
        clawt_ipc_payload_boolean(agent, "held", FALSE),
        clawt_ipc_payload_boolean(agent, "draining", FALSE));

    if (held != NULL)
        clawt_lrg_take(badges, clawt_lrg_badge(held, "warn"));

    if (agent != NULL && json_object_has_member(agent, "failover") &&
        JSON_NODE_HOLDS_OBJECT(json_object_get_member(agent, "failover")))
        fallback = clawt_failover_badge(
            json_object_get_object_member(agent, "failover"));

    if (fallback != NULL)
        clawt_lrg_take(badges, clawt_lrg_badge(fallback, "warn"));

    clawt_lrg_take(badges, clawt_lrg_badge(
        clawt_ipc_payload_boolean(agent, "connected", FALSE)
            ? "linked" : "not linked",
        clawt_ipc_payload_boolean(agent, "connected", FALSE)
            ? "good" : "neutral"));
    clawt_lrg_take(ins.glance, badges);

    /*
     * And the sentence: whose backend gave out, why, and when it comes
     * back.  Redrawn by every agent.* event, agent.failover among them.
     */
    if (fallback != NULL) {
        const gchar *message = clawt_ipc_payload_string(agent,
                                                        "failover_message");

        clawt_lrg_take(ins.glance, clawt_lrg_paragraph(
            message != NULL ? message : fallback, "caption"));
    }

    clawt_lrg_take(ins.glance, info_row("Id",
        clawt_ipc_payload_string(agent, "id")));

    if (clawt_ipc_payload_string(agent, "detail") != NULL)
        clawt_lrg_take(ins.glance, info_row("Why",
            clawt_ipc_payload_string(agent, "detail")));

    clawt_lrg_take(ins.glance, info_row("Can do",
        clawt_ipc_payload_string(agent, "caps")));
    clawt_lrg_take(ins.glance, info_row("Model", clawt_ipc_payload_string(
        agent, "model")));
    clawt_lrg_take(ins.glance, info_row("Computer", clawt_ipc_payload_string(
        agent, "computer")));

    /*
     * What the persona costs, always while there is a count -- a size
     * somebody can watch is the point -- and the sentence naming the
     * files only past the threshold.  An agent whose identity outgrows
     * one argv word cannot start a fresh session, and the kernel's
     * refusal names neither the files nor the limit.
     */
    identity_text = clawt_lrg_agent_identity_text(identity);

    if (identity_text != NULL)
        clawt_lrg_take(ins.glance, info_row("Identity", identity_text));

    if (clawt_ipc_payload_string(identity, "verdict") != NULL) {
        LrgWidget *note = clawt_lrg_hbox(8.0f);

        lrg_widget_add_class(note, "clawt-identity-size");
        clawt_lrg_take(note, clawt_lrg_badge(
            g_strcmp0(clawt_lrg_agent_identity_tone(identity), "bad") == 0
                ? "too large" : "filling up",
            clawt_lrg_agent_identity_tone(identity)));
        {
            LrgWidget *verdict = clawt_lrg_paragraph(
                clawt_ipc_payload_string(identity, "verdict"), NULL);

            /* the part of the row that wraps, so it is the part that gives way */
            lrg_widget_set_fill_weight(verdict, 1.0f);
            clawt_lrg_take(note, verdict);
        }
        clawt_lrg_take(ins.glance, note);
    }

    /* Running it, each button offered only where it can work. */
    clawt_lrg_agent_verbs(state, verbs);

    for (i = 0; i < CLAWT_LRG_AGENT_VERBS; i++) {
        LrgWidget *button = clawt_lrg_button(
            verbs[i].label, i == 0 ? "primary" : NULL,
            G_CALLBACK(on_verb), app);

        g_object_set_data(G_OBJECT(button), "clawt-kind",
                          (gpointer)verbs[i].kind);
        lrg_widget_set_enabled(button, verbs[i].enabled);
        clawt_lrg_take(actions, button);
    }

    /* A disabled control says why: there are no tooltips to hide it in. */
    for (i = 0; i < CLAWT_LRG_AGENT_VERBS; i++) {
        if (!verbs[i].enabled && verbs[i].why_not != NULL) {
            g_autofree gchar *why = g_strdup_printf("%s: %s", verbs[i].label,
                                                    verbs[i].why_not);

            clawt_lrg_take(actions, clawt_lrg_label(why, "caption"));
        }
    }

    clawt_lrg_take(ins.glance, actions);
    clawt_lrg_app_wake(app);
}

/* ── Usage ────────────────────────────────────────────────────────── */

static void
on_usage(ClawtLrgApp *app, JsonNode *reply, const GError *error,
         gpointer user_data)
{
    LrgWidget *label = GPOINTER_TO_UINT(user_data) ? ins.usage_total
                                                   : ins.usage_today;
    g_autofree gchar *said = clawt_lrg_agent_usage_text(reply, app->agent_id);

    if (label == NULL)
        return;

    {
        g_autofree gchar *escaped = clawt_lrg_markdown_escape(
            said != NULL ? said
            : reply == NULL ? "could not be read" : "nothing recorded");

        lrg_markdown_label_set_markdown(LRG_MARKDOWN_LABEL(label), escaped);
    }
    clawt_lrg_app_wake(app);
}

static JsonNode *
usage_payload(gboolean today)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    gint64 since = 0;

    if (today) {
        g_autoptr(GDateTime) now = g_date_time_new_now_local();
        g_autoptr(GDateTime) midnight = g_date_time_new_local(
            g_date_time_get_year(now), g_date_time_get_month(now),
            g_date_time_get_day_of_month(now), 0, 0, 0.0);

        since = g_date_time_to_unix(midnight);
    }

    /* `since` is unix seconds and read as an integer by the daemon. */
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "since");
    json_builder_add_int_value(builder, since);
    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

static void
draw_usage(ClawtLrgApp *app, guint index)
{
    LrgWidget *box = section_card(
        app, ins.body, "Usage",
        "What this agent's turns have cost, as its CLI reported it. A cost "
        "the CLI did not report is said, never shown as free.", index);
    LrgWidget *row;

    /*
     * The figures are the row's wrapping subtitle, not a suffix: "$13.38
     * across 412 turns, 1.2M tokens in…" is a sentence, and as a one-line
     * suffix it was the one thing in the row that could not give way, so
     * a narrow window ran it past the card's edge.
     */
    row = clawt_lrg_row("Today", "\xe2\x80\xa6");
    ins.usage_today = clawt_lrg_row_subtitle(row);
    clawt_lrg_take(box, row);

    row = clawt_lrg_row("All time", "\xe2\x80\xa6");
    ins.usage_total = clawt_lrg_row_subtitle(row);
    clawt_lrg_take(box, row);

    clawt_lrg_request(app, "usage.summary", usage_payload(TRUE),
                      CLAWT_LRG_GUARDED | CLAWT_LRG_QUIET, on_usage,
                      GUINT_TO_POINTER(0), NULL);
    clawt_lrg_request(app, "usage.summary", usage_payload(FALSE),
                      CLAWT_LRG_GUARDED | CLAWT_LRG_QUIET, on_usage,
                      GUINT_TO_POINTER(1), NULL);
}

/* ── The profile picture ──────────────────────────────────────────── */

typedef struct {
    ClawtLrgApp *app;
    gchar       *agent_id;
    gboolean     cleared;
} AvatarJob;

static void
avatar_job_free(gpointer data)
{
    AvatarJob *job = data;

    g_free(job->agent_id);
    g_free(job);
}

static void
on_avatar_changed(ClawtLrgApp *app, JsonNode *reply, const GError *error,
                  gpointer user_data)
{
    AvatarJob *job = user_data;

    if (reply == NULL)
        return;

    /*
     * The cache is told now, not when the daemon's agent.changed echo
     * arrives: this window asked for the change and should draw it.
     */
    clawt_lrg_avatar_invalidate(job->agent_id);

    if (!job->cleared)
        clawt_lrg_toast(app, "Profile picture updated.");

    clawt_lrg_refresh_fleet(app);
    refresh(app);
}

/*
 * Bytes, and only bytes: the daemon writes them into the agent's own
 * directory and picks the extension by sniffing them, never from
 * anything this client claims about the file.
 */
static void
on_avatar_file_chosen(GObject *source, GAsyncResult *result,
                      gpointer user_data)
{
    AvatarJob *job = user_data;
    ClawtLrgApp *app = job->app;
    g_autoptr(GFile) file = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *contents = NULL;
    g_autofree gchar *encoded = NULL;
    gsize length = 0;

    file = lrg_file_dialog_open_finish(LRG_FILE_DIALOG(source), result,
                                       &error);

    if (file == NULL) {
        /* Backing out of the dialog is not a failure worth a toast. */
        if (error != NULL && !g_error_matches(error, LRG_FILE_DIALOG_ERROR,
                                              LRG_FILE_DIALOG_ERROR_DISMISSED))
            clawt_lrg_toast(app, error->message);

        avatar_job_free(job);
        return;
    }

    if (!g_file_load_contents(file, NULL, &contents, &length, NULL, &error)) {
        clawt_lrg_toast(app, error->message);
        avatar_job_free(job);
        return;
    }

    encoded = g_base64_encode((const guchar *)contents, length);

    clawt_lrg_request(app, "agent.avatar_set",
                      clawt_ipc_payload_new("agent", job->agent_id,
                                            "data", encoded, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_avatar_changed, job,
                      avatar_job_free);
}

static void
on_avatar_choose(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    g_autoptr(LrgFileDialog) dialog = lrg_file_dialog_new();
    AvatarJob *job;

    if (app->agent_id == NULL)
        return;

    job = g_new0(AvatarJob, 1);
    job->app = app;
    job->agent_id = g_strdup(app->agent_id);

    /*
     * No filter: the daemon sniffs the bytes and refuses anything that
     * is not PNG, JPEG or WebP, which is a more honest answer than a
     * filter guessing from a name a file may not even have.
     */
    lrg_file_dialog_set_title(dialog, "Choose a profile picture");
    lrg_file_dialog_set_accept_label(dialog, "Use this picture");

    g_main_context_push_thread_default(app->context);
    lrg_file_dialog_open(dialog, LRG_WIDGET(app->canvas), NULL,
                         on_avatar_file_chosen, job);
    g_main_context_pop_thread_default(app->context);
    clawt_lrg_app_wake(app);
}

static void
on_avatar_clear(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    AvatarJob *job;

    if (app->agent_id == NULL)
        return;

    job = g_new0(AvatarJob, 1);
    job->app = app;
    job->agent_id = g_strdup(app->agent_id);
    job->cleared = TRUE;
    clawt_lrg_request(app, "agent.avatar_clear",
                      clawt_ipc_payload_new("agent", app->agent_id, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_avatar_changed, job,
                      avatar_job_free);
}

/*
 * The face, and the ways to change it.  A card of its own rather than a
 * row among the settings: a picture is chosen, not typed, and the face
 * is its own preview.
 */
static void
build_avatar_group(ClawtLrgApp *app, JsonObject *agent, guint index)
{
    const gchar *agent_id = clawt_ipc_payload_string(agent, "id");
    const gchar *name = clawt_ipc_payload_string(agent, "name");
    const gchar *configured = clawt_ipc_payload_string(agent, "avatar");
    gboolean has_avatar = clawt_ipc_payload_boolean(agent, "has_avatar",
                                                    FALSE);
    LrgWidget *box = section_card(
        app, ins.body, "Profile picture",
        has_avatar
            ? ((configured != NULL && *configured != '\0')
                   ? "Set from agents.avatar."
                   : "Auto-detected: profile-picture.png (or .jpg, .jpeg, or "
                     ".webp) in the agent's own directory.")
            : "No picture yet, so initials are shown instead.", index);
    LrgWidget *row = clawt_lrg_row(
        "Picture", "PNG, JPEG or WebP -- the file's own bytes decide, not "
                   "its name.");
    LrgWidget *clear;

    clawt_lrg_row_add_prefix(row, clawt_lrg_avatar(
        app, (name != NULL) ? name : agent_id, agent_id, has_avatar,
        clawt_ipc_payload_string(agent, "color"), 64.0f,
        g_strcmp0(clawt_ipc_payload_string(agent, "state"), "running") == 0));
    clawt_lrg_row_add_suffix(row, clawt_lrg_button(
        "Choose\xe2\x80\xa6", NULL, G_CALLBACK(on_avatar_choose), app));

    clear = clawt_lrg_button("Remove", "flat", G_CALLBACK(on_avatar_clear),
                             app);
    lrg_widget_set_enabled(clear, has_avatar);
    clawt_lrg_row_add_suffix(row, clear);

    clawt_lrg_take(box, row);
}

/* ── Choosers ─────────────────────────────────────────────────────── */

static void on_chooser_changed(LrgComboBox *combo, gpointer user_data);

static FieldView *
find_view(ClawtLrgAgentFieldKind kind)
{
    guint i;

    for (i = 0; ins.views != NULL && i < ins.views->len; i++) {
        FieldView *view = g_ptr_array_index(ins.views, i);

        if (view->field->kind == kind)
            return view;
    }

    return NULL;
}

/* The note a chooser shows under itself for what is selected. */
static void
update_note(FieldView *view)
{
    const gchar *id;
    g_autofree gchar *text = NULL;

    if (view->note == NULL)
        return;

    id = clawt_lrg_combo_id(view->control);

    switch (view->field->kind) {
    case CLAWT_LRG_AGENT_FIELD_IMAGE:
        text = clawt_lrg_agent_image_note(ins.images, id);
        break;

    case CLAWT_LRG_AGENT_FIELD_DISK:
        text = clawt_lrg_agent_disk_note(ins.disks, id);
        break;

    case CLAWT_LRG_AGENT_FIELD_PROVIDER:
        text = g_strdup(clawt_lrg_agent_provider_note(ins.catalog, id));
        break;

    case CLAWT_LRG_AGENT_FIELD_ENTRY:
    case CLAWT_LRG_AGENT_FIELD_PROSE:
    case CLAWT_LRG_AGENT_FIELD_SWITCH:
    case CLAWT_LRG_AGENT_FIELD_ENUM:
    case CLAWT_LRG_AGENT_FIELD_SECRET:
    case CLAWT_LRG_AGENT_FIELD_LIST:
    case CLAWT_LRG_AGENT_FIELD_TEAM:
    case CLAWT_LRG_AGENT_FIELD_ACCOUNT:
    case CLAWT_LRG_AGENT_FIELD_SHARED:
    case CLAWT_LRG_AGENT_FIELD_MODEL:
    case CLAWT_LRG_AGENT_FIELD_EFFORT:
    case CLAWT_LRG_AGENT_FIELD_COMPUTER_TYPE:
    case CLAWT_LRG_AGENT_FIELD_RESOLUTION:
        break;
    }

    lrg_label_set_text(LRG_LABEL(view->note), text != NULL ? text : "");
    lrg_widget_set_visible(view->note, text != NULL && *text != '\0');
}

/*
 * Fills a chooser's slot from @choices: the combo, a free entry when the
 * list has "Other…" (shown only while it is selected), and a note.
 */
static void
fill_chooser(FieldView *view, ClawtLrgAgentChoices *choices)
{
    gboolean other = clawt_lrg_agent_choices_has(choices, CLAWT_LRG_AGENT_CHOICE_OTHER);

    clawt_lrg_clear(view->slot);
    view->other = NULL;
    view->note = NULL;

    view->control = clawt_lrg_combo(clawt_lrg_agent_choices_ids(choices),
                                    clawt_lrg_agent_choices_labels(choices),
                                    choices->selected);
    clawt_lrg_add(view->slot, view->control);
    g_object_unref(view->control);

    if (other) {
        view->other = clawt_lrg_entry(
            choices->other,
            view->field->kind == CLAWT_LRG_AGENT_FIELD_DISK
                ? "Path to a qcow2"
                : view->field->kind == CLAWT_LRG_AGENT_FIELD_MODEL
                    ? "Model name" : "Image reference");
        lrg_widget_set_visible(view->other,
                               g_strcmp0(choices->selected,
                                         CLAWT_LRG_AGENT_CHOICE_OTHER) == 0);
        clawt_lrg_add(view->slot, view->other);
        g_object_unref(view->other);
    }

    if (view->field->kind == CLAWT_LRG_AGENT_FIELD_IMAGE ||
        view->field->kind == CLAWT_LRG_AGENT_FIELD_DISK ||
        view->field->kind == CLAWT_LRG_AGENT_FIELD_PROVIDER) {
        view->note = clawt_lrg_label("", "caption");
        clawt_lrg_add(view->slot, view->note);
        g_object_unref(view->note);
        update_note(view);
    }

    /* Connected after the selection is set, so building is not a change. */
    g_signal_connect(view->control, "changed",
                     G_CALLBACK(on_chooser_changed), view);
}

/* The model list belongs to a provider; it follows the provider combo. */
static void
refill_models(void)
{
    FieldView *provider = find_view(CLAWT_LRG_AGENT_FIELD_PROVIDER);
    FieldView *model = find_view(CLAWT_LRG_AGENT_FIELD_MODEL);
    const gchar *chosen;
    g_autoptr(ClawtLrgAgentChoices) choices = NULL;

    if (provider == NULL || model == NULL)
        return;

    chosen = clawt_lrg_combo_id(provider->control);

    /*
     * The agent's own model only under its own provider: "sonnet" means
     * nothing to Ollama, so switching provider starts from that
     * provider's first model rather than carrying a name across.
     */
    choices = clawt_lrg_agent_model_choices(
        ins.catalog, chosen,
        g_strcmp0(chosen, provider->field->value) == 0 ? model->field->value
                                                       : NULL);
    fill_chooser(model, choices);
}

static void
on_chooser_changed(LrgComboBox *combo, gpointer user_data)
{
    FieldView *view = user_data;

    if (view->other != NULL)
        lrg_widget_set_visible(view->other,
                               g_strcmp0(clawt_lrg_combo_id(view->control),
                                         CLAWT_LRG_AGENT_CHOICE_OTHER) == 0);

    update_note(view);

    if (view->field->kind == CLAWT_LRG_AGENT_FIELD_PROVIDER)
        refill_models();
}

static void
on_choices_reply(ClawtLrgApp *app, JsonNode *reply, const GError *error,
                 gpointer user_data)
{
    ClawtLrgAgentFieldKind kind = (ClawtLrgAgentFieldKind)GPOINTER_TO_UINT(user_data);
    FieldView *view = find_view(kind);
    g_autoptr(ClawtLrgAgentChoices) choices = NULL;

    /*
     * No answer leaves the chooser as it was drawn -- holding the
     * current value -- rather than an empty list a save would read as a
     * change.
     */
    if (reply == NULL || view == NULL)
        return;

    switch (kind) {
    case CLAWT_LRG_AGENT_FIELD_TEAM:
        choices = clawt_lrg_agent_team_choices(reply, view->field->value);
        break;

    case CLAWT_LRG_AGENT_FIELD_ACCOUNT:
        choices = clawt_lrg_agent_account_choices(reply, view->field->value);
        break;

    case CLAWT_LRG_AGENT_FIELD_SHARED:
        choices = clawt_lrg_agent_shared_choices(reply, view->field->value);
        break;

    case CLAWT_LRG_AGENT_FIELD_IMAGE:
        g_clear_pointer(&ins.images, json_node_unref);
        ins.images = json_node_copy(reply);
        choices = clawt_lrg_agent_image_choices(reply, view->field->value);
        break;

    case CLAWT_LRG_AGENT_FIELD_DISK:
        g_clear_pointer(&ins.disks, json_node_unref);
        ins.disks = json_node_copy(reply);
        choices = clawt_lrg_agent_disk_choices(reply, view->field->value);
        break;

    case CLAWT_LRG_AGENT_FIELD_PROVIDER:
        g_clear_pointer(&ins.catalog, json_node_unref);
        ins.catalog = json_node_copy(reply);
        choices = clawt_lrg_agent_provider_choices(reply, view->field->value);
        break;

    case CLAWT_LRG_AGENT_FIELD_ENTRY:
    case CLAWT_LRG_AGENT_FIELD_PROSE:
    case CLAWT_LRG_AGENT_FIELD_SWITCH:
    case CLAWT_LRG_AGENT_FIELD_ENUM:
    case CLAWT_LRG_AGENT_FIELD_SECRET:
    case CLAWT_LRG_AGENT_FIELD_LIST:
    case CLAWT_LRG_AGENT_FIELD_MODEL:
    case CLAWT_LRG_AGENT_FIELD_EFFORT:
    case CLAWT_LRG_AGENT_FIELD_COMPUTER_TYPE:
    case CLAWT_LRG_AGENT_FIELD_RESOLUTION:
        return;
    }

    fill_chooser(view, choices);

    if (kind == CLAWT_LRG_AGENT_FIELD_PROVIDER) {
        refill_models();

        /* the fallback entries draw from the same catalogue */
        redraw_fallbacks(app);
    }

    clawt_lrg_app_wake(app);
}

/* The answers each chooser is built from, asked once per draw. */
static void
ask_for_choices(ClawtLrgApp *app)
{
    if (find_view(CLAWT_LRG_AGENT_FIELD_TEAM) != NULL)
        clawt_lrg_request(app, "team.list", NULL,
                          CLAWT_LRG_GUARDED | CLAWT_LRG_QUIET,
                          on_choices_reply,
                          GUINT_TO_POINTER(CLAWT_LRG_AGENT_FIELD_TEAM), NULL);

    /* With the agent: the daemon applies the check agent.set will. */
    if (find_view(CLAWT_LRG_AGENT_FIELD_ACCOUNT) != NULL)
        clawt_lrg_request(app, "account.list",
                          clawt_ipc_payload_new("agent", app->agent_id, NULL),
                          CLAWT_LRG_GUARDED | CLAWT_LRG_QUIET,
                          on_choices_reply,
                          GUINT_TO_POINTER(CLAWT_LRG_AGENT_FIELD_ACCOUNT), NULL);

    if (find_view(CLAWT_LRG_AGENT_FIELD_SHARED) != NULL)
        clawt_lrg_request(app, "computer.shared", NULL,
                          CLAWT_LRG_GUARDED | CLAWT_LRG_QUIET,
                          on_choices_reply,
                          GUINT_TO_POINTER(CLAWT_LRG_AGENT_FIELD_SHARED), NULL);

    /*
     * refresh: the providers' own lists where the daemon can get them; it
     * falls back to its table with no key or no network, so this never
     * draws an empty chooser.  Asynchronous, because that refresh may
     * leave the machine.
     */
    if (find_view(CLAWT_LRG_AGENT_FIELD_PROVIDER) != NULL)
        clawt_lrg_request(app, "model.list",
                          clawt_ipc_payload_new("refresh", "true", NULL),
                          CLAWT_LRG_GUARDED | CLAWT_LRG_QUIET,
                          on_choices_reply,
                          GUINT_TO_POINTER(CLAWT_LRG_AGENT_FIELD_PROVIDER), NULL);

    if (find_view(CLAWT_LRG_AGENT_FIELD_IMAGE) != NULL)
        clawt_lrg_request(app, "image.list", NULL,
                          CLAWT_LRG_GUARDED | CLAWT_LRG_QUIET,
                          on_choices_reply,
                          GUINT_TO_POINTER(CLAWT_LRG_AGENT_FIELD_IMAGE), NULL);

    if (find_view(CLAWT_LRG_AGENT_FIELD_DISK) != NULL)
        clawt_lrg_request(app, "image.vm_list", NULL,
                          CLAWT_LRG_GUARDED | CLAWT_LRG_QUIET,
                          on_choices_reply,
                          GUINT_TO_POINTER(CLAWT_LRG_AGENT_FIELD_DISK), NULL);
}

/* ── The fallback list ────────────────────────────────────────────── */

/*
 * One `model.fallbacks` entry's controls.  The model and account combos
 * live in slots of their own because they are refilled when the
 * provider changes: a model and a login both belong to one CLI.
 */
typedef struct {
    LrgWidget *provider;
    LrgWidget *model_slot;
    LrgWidget *model;
    LrgWidget *other;
    LrgWidget *account_slot;
    LrgWidget *account;
    gchar     *drawn_provider;   /* the provider the slots were filled for */
    gchar     *drawn_model;
    gchar     *drawn_account;
} FallbackView;

static void
fallback_view_free(gpointer data)
{
    FallbackView *view = data;

    g_free(view->drawn_provider);
    g_free(view->drawn_model);
    g_free(view->drawn_account);
    g_free(view);
}

static void
on_fallback_model_changed(LrgComboBox *combo, gpointer user_data)
{
    FallbackView *view = user_data;

    if (view->other != NULL)
        lrg_widget_set_visible(view->other,
                               g_strcmp0(clawt_lrg_combo_id(view->model),
                                         CLAWT_LRG_AGENT_CHOICE_OTHER) == 0);
}

/*
 * The model and account combos for @provider.  The entry's own model
 * and login are offered back only under its own provider: "sonnet"
 * means nothing to Codex, and a Claude login is not a Codex one.
 */
static void
fill_fallback_slots(FallbackView *view, const gchar *provider)
{
    gboolean own = g_strcmp0(provider, view->drawn_provider) == 0;
    g_autoptr(ClawtLrgAgentChoices) models = clawt_lrg_agent_model_choices(
        ins.catalog, provider, own ? view->drawn_model : NULL);
    g_autoptr(ClawtLrgAgentChoices) accounts =
        clawt_lrg_agent_fallback_account_choices(
            ins.accounts, provider, own ? view->drawn_account : NULL);

    clawt_lrg_clear(view->model_slot);
    view->model = clawt_lrg_combo(clawt_lrg_agent_choices_ids(models),
                                  clawt_lrg_agent_choices_labels(models),
                                  models->selected);
    clawt_lrg_add(view->model_slot, view->model);
    g_object_unref(view->model);

    view->other = clawt_lrg_entry(models->other, "Model name");
    lrg_widget_set_visible(view->other,
                           g_strcmp0(models->selected,
                                     CLAWT_LRG_AGENT_CHOICE_OTHER) == 0);
    clawt_lrg_add(view->model_slot, view->other);
    g_object_unref(view->other);
    g_signal_connect(view->model, "changed",
                     G_CALLBACK(on_fallback_model_changed), view);

    clawt_lrg_clear(view->account_slot);
    view->account = clawt_lrg_combo(clawt_lrg_agent_choices_ids(accounts),
                                    clawt_lrg_agent_choices_labels(accounts),
                                    accounts->selected);
    clawt_lrg_add(view->account_slot, view->account);
    g_object_unref(view->account);
}

static void
on_fallback_provider_changed(LrgComboBox *combo, gpointer user_data)
{
    FallbackView *view = user_data;

    fill_fallback_slots(view, clawt_lrg_combo_id(view->provider));
}

/*
 * What the editor holds, in order.  A model left blank is kept as an
 * empty one so that moving another entry does not lose this one;
 * clawt_lrg_agent_fallback_spell() is what refuses it at save.
 *
 * Returns: (transfer full) (element-type ClawtModelFallback): the entries
 */
static GPtrArray *
read_fallbacks(void)
{
    GPtrArray *list = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_model_fallback_free);
    guint i;

    for (i = 0; ins.fallback_views != NULL && i < ins.fallback_views->len;
         i++) {
        FallbackView *view = g_ptr_array_index(ins.fallback_views, i);
        g_autofree gchar *model = clawt_lrg_agent_choice_value(
            clawt_lrg_combo_id(view->model),
            clawt_lrg_entry_text(view->other));
        const gchar *provider = clawt_lrg_combo_id(view->provider);

        g_ptr_array_add(list, clawt_model_fallback_new(
            provider != NULL ? provider : "", model != NULL ? model : "",
            clawt_lrg_combo_id(view->account)));
    }

    return list;
}

static void draw_fallbacks(ClawtLrgApp *app, GPtrArray *list);

/* An edit waiting for the frame after the click that asked for it. */
static gboolean
on_fallbacks_redraw(gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    g_autoptr(GPtrArray) list = g_steal_pointer(&ins.fallback_pending);

    if (list != NULL && ins.fallbacks != NULL)
        draw_fallbacks(app, list);

    clawt_lrg_app_wake(app);
    return G_SOURCE_REMOVE;
}

/*
 * Add, remove and move.  The list is read back from the controls and
 * drawn again, so an unsaved choice in one entry survives moving
 * another; nothing is sent until "Save changes".  Drawn on the next
 * iteration rather than here, because drawing again destroys the button
 * whose handler this is.
 */
static void
on_fallback_edit(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    const gchar *op = g_object_get_data(G_OBJECT(button), "clawt-op");
    guint index = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button),
                                                     "clawt-index"));
    const gchar *provider = clawt_ipc_payload_string(shown_object("settings"),
                                                     "model.provider");
    g_autoptr(GPtrArray) list = read_fallbacks();
    GSource *source;

    if (!clawt_lrg_agent_fallback_edit(
            list, op, index,
            (provider != NULL && *provider != '\0')
                ? provider : clawt_model_catalog_default_provider()))
        return;

    g_clear_pointer(&ins.fallback_pending, g_ptr_array_unref);
    ins.fallback_pending = g_steal_pointer(&list);

    source = g_idle_source_new();
    g_source_set_callback(source, on_fallbacks_redraw, app, NULL);
    clawt_lrg_app_attach(app, source);
    g_source_unref(source);
}

static LrgWidget *
fallback_button(ClawtLrgApp *app, const gchar *label, const gchar *style,
                const gchar *op, guint index, gboolean enabled)
{
    LrgWidget *button = clawt_lrg_button(label, style,
                                         G_CALLBACK(on_fallback_edit), app);

    g_object_set_data(G_OBJECT(button), "clawt-op", (gpointer)op);
    g_object_set_data(G_OBJECT(button), "clawt-index", GUINT_TO_POINTER(index));
    lrg_widget_set_enabled(button, enabled);

    return button;
}

/* The entries, drawn into the card from @list. */
static void
draw_fallbacks(ClawtLrgApp *app, GPtrArray *list)
{
    guint i;

    clawt_lrg_clear(ins.fallbacks);
    g_clear_pointer(&ins.fallback_views, g_ptr_array_unref);
    ins.fallback_views = g_ptr_array_new_with_free_func(fallback_view_free);

    if (list->len == 0)
        clawt_lrg_take(ins.fallbacks, clawt_lrg_paragraph(
            "No fallbacks: at a limit this agent waits for the reset.",
            "muted"));

    for (i = 0; i < list->len; i++) {
        ClawtModelFallback *entry = g_ptr_array_index(list, i);
        FallbackView *view = g_new0(FallbackView, 1);
        g_autoptr(ClawtLrgAgentChoices) providers =
            clawt_lrg_agent_provider_choices(ins.catalog, entry->provider);
        LrgWidget *heading = clawt_lrg_hbox(8.0f);
        LrgWidget *form = clawt_lrg_form_new();
        g_autofree gchar *title = g_strdup_printf("Fallback %u", i + 1);
        LrgWidget *label = clawt_lrg_label(title, "heading");

        view->drawn_provider = g_strdup(entry->provider);
        view->drawn_model = g_strdup(entry->model);
        view->drawn_account = g_strdup(entry->account != NULL ? entry->account
                                                              : "");
        g_ptr_array_add(ins.fallback_views, view);

        /*
         * Each entry's own order buttons beside its own heading, so which
         * entry a button moves is never a question.
         */
        lrg_widget_set_fill_weight(label, 1.0f);
        clawt_lrg_take(heading, label);
        clawt_lrg_take(heading, fallback_button(app, "Earlier", "flat", "up",
                                                i, i > 0));
        clawt_lrg_take(heading, fallback_button(app, "Later", "flat", "down",
                                                i, i + 1 < list->len));
        clawt_lrg_take(heading, fallback_button(app, "Remove", "danger",
                                                "remove", i, TRUE));
        clawt_lrg_take(ins.fallbacks, heading);

        view->provider = clawt_lrg_combo(clawt_lrg_agent_choices_ids(providers),
                                         clawt_lrg_agent_choices_labels(providers),
                                         providers->selected);
        clawt_lrg_form_add(form, "Provider", g_object_ref(view->provider),
                           NULL);
        g_object_unref(view->provider);

        view->model_slot = clawt_lrg_vbox(4.0f);
        clawt_lrg_form_add(form, "Model", g_object_ref(view->model_slot), NULL);
        g_object_unref(view->model_slot);

        view->account_slot = clawt_lrg_vbox(4.0f);
        clawt_lrg_form_add(form, "Account", g_object_ref(view->account_slot),
                           "A login for this entry's CLI, or its own.");
        g_object_unref(view->account_slot);

        fill_fallback_slots(view, entry->provider);

        /* Connected after the selection is set, so building is not a change. */
        g_signal_connect(view->provider, "changed",
                         G_CALLBACK(on_fallback_provider_changed), view);

        clawt_lrg_take(ins.fallbacks, form);
    }

    clawt_lrg_app_wake(app);
}

/* model.list or account.list answered: the same entries, better filled. */
static void
redraw_fallbacks(ClawtLrgApp *app)
{
    g_autoptr(GPtrArray) list = NULL;

    if (ins.fallbacks == NULL || ins.fallback_views == NULL)
        return;

    list = read_fallbacks();
    draw_fallbacks(app, list);
}

static void
on_fallback_accounts(ClawtLrgApp *app, JsonNode *reply, const GError *error,
                     gpointer user_data)
{
    if (reply == NULL)
        return;

    g_clear_pointer(&ins.accounts, json_node_unref);
    ins.accounts = json_node_copy(reply);
    redraw_fallbacks(app);
}

/*
 * The card, under the Model group whose backend it falls back from.
 * Shown as the reason rather than as an editor when the list cannot be
 * read: an empty editor would save the empty list over the real one.
 */
static guint
build_fallbacks(ClawtLrgApp *app, guint index)
{
    const gchar *spelled = clawt_ipc_payload_string(shown_object("settings"),
                                                    "model.fallbacks");
    g_autoptr(GError) error = NULL;
    g_autoptr(GPtrArray) list = clawt_model_fallback_parse_list(spelled,
                                                                &error);
    LrgWidget *box = section_card(
        app, ins.body, "Fallbacks",
        "Where this agent's conversations continue, in this order, when its "
        "own backend hits a usage limit, a rate limit, a sign-out or an "
        "outage. It comes back at the reset. A different CLI or login starts "
        "a fresh conversation; another sign-in to the same subscription is "
        "refused.", index++);

    g_clear_pointer(&ins.fallbacks_saved, g_free);
    g_clear_pointer(&ins.fallback_views, g_ptr_array_unref);
    g_clear_pointer(&ins.fallback_pending, g_ptr_array_unref);
    ins.fallbacks = NULL;

    if (list == NULL) {
        g_autofree gchar *why = g_strdup_printf(
            "%s. Fix it in clawtilla.yaml or with `clawtilla agent set`; this "
            "page will not save over a list it cannot show.", error->message);

        clawt_lrg_take(box, clawt_lrg_badge("cannot read the list", "bad"));
        clawt_lrg_take(box, clawt_lrg_paragraph(why, NULL));
        return index;
    }

    ins.fallbacks_saved = clawt_model_fallback_list_to_string(list);

    ins.fallbacks = clawt_lrg_vbox(10.0f);
    clawt_lrg_add(box, ins.fallbacks);
    g_object_unref(ins.fallbacks);

    clawt_lrg_take(box, fallback_button(app, "Add a fallback", NULL, "add", 0,
                                        TRUE));

    draw_fallbacks(app, list);

    clawt_lrg_request(app, "account.list", NULL,
                      CLAWT_LRG_GUARDED | CLAWT_LRG_QUIET,
                      on_fallback_accounts, NULL, NULL);

    return index;
}

/* ── The settings form ────────────────────────────────────────────── */

/* The control for one field, drawn before any chooser's reply lands. */
static void
build_control(FieldView *view)
{
    ClawtLrgAgentField *field = view->field;
    g_autoptr(ClawtLrgAgentChoices) choices = NULL;

    switch (field->kind) {
    case CLAWT_LRG_AGENT_FIELD_ENTRY:
        view->control = clawt_lrg_entry(field->value, NULL);
        break;

    case CLAWT_LRG_AGENT_FIELD_LIST:
        view->control = clawt_lrg_entry(field->value, "comma separated");
        break;

    case CLAWT_LRG_AGENT_FIELD_SECRET:
        /*
         * Never prefilled: the daemon reports only that one is set, and
         * a placeholder in the field would be posted back as the value.
         */
        view->control = clawt_lrg_entry(
            NULL, g_strcmp0(field->value, "(set)") == 0
                      ? "set; type to replace" : "a secret reference");
        break;

    case CLAWT_LRG_AGENT_FIELD_PROSE:
        view->control = clawt_lrg_editor(field->value, NULL);
        lrg_widget_set_min_size(view->control, 0.0f, 140.0f);
        break;

    case CLAWT_LRG_AGENT_FIELD_SWITCH:
        view->control = clawt_lrg_switch(field->label,
                                         g_strcmp0(field->value, "true") == 0);
        break;

    case CLAWT_LRG_AGENT_FIELD_ENUM:
        choices = clawt_lrg_agent_enum_choices(field->enum_type, field->value);
        break;

    case CLAWT_LRG_AGENT_FIELD_EFFORT:
        choices = clawt_lrg_agent_effort_choices(field->value);
        break;

    case CLAWT_LRG_AGENT_FIELD_COMPUTER_TYPE:
        choices = clawt_lrg_agent_computer_type_choices(field->value);
        break;

    case CLAWT_LRG_AGENT_FIELD_RESOLUTION:
        choices = clawt_lrg_agent_resolution_choices(field->value);
        break;

    /* The current value alone, until the daemon says what else there is. */
    case CLAWT_LRG_AGENT_FIELD_TEAM:
        choices = clawt_lrg_agent_team_choices(NULL, field->value);
        break;

    case CLAWT_LRG_AGENT_FIELD_ACCOUNT:
        choices = clawt_lrg_agent_account_choices(NULL, field->value);
        break;

    case CLAWT_LRG_AGENT_FIELD_SHARED:
        choices = clawt_lrg_agent_shared_choices(NULL, field->value);
        break;

    case CLAWT_LRG_AGENT_FIELD_PROVIDER:
        choices = clawt_lrg_agent_provider_choices(NULL, field->value);
        break;

    case CLAWT_LRG_AGENT_FIELD_MODEL:
        choices = clawt_lrg_agent_model_choices(NULL, NULL, field->value);
        break;

    case CLAWT_LRG_AGENT_FIELD_IMAGE:
        choices = clawt_lrg_agent_image_choices(NULL, field->value);
        break;

    case CLAWT_LRG_AGENT_FIELD_DISK:
        choices = clawt_lrg_agent_disk_choices(NULL, field->value);
        break;
    }

    if (choices != NULL) {
        fill_chooser(view, choices);
        return;
    }

    clawt_lrg_add(view->slot, view->control);
    g_object_unref(view->control);
}

/* What one control holds, as agent.set takes it; NULL leaves it. */
static gchar *
view_value(FieldView *view)
{
    switch (view->field->kind) {
    case CLAWT_LRG_AGENT_FIELD_ENTRY:
    case CLAWT_LRG_AGENT_FIELD_LIST:
        return g_strstrip(g_strdup(clawt_lrg_entry_text(view->control)));

    case CLAWT_LRG_AGENT_FIELD_SECRET: {
        g_autofree gchar *typed =
            g_strstrip(g_strdup(clawt_lrg_entry_text(view->control)));

        /*
         * Empty means "leave it": clearing a secret by accident costs an
         * authorization somebody cannot see the reason for.
         */
        return (*typed == '\0') ? NULL : g_steal_pointer(&typed);
    }

    case CLAWT_LRG_AGENT_FIELD_PROSE:
        return clawt_lrg_editor_text(view->control);

    /*
     * Written either way: a switch turned off is a value of its own, and
     * skipping it would make a setting impossible to turn back off.
     */
    case CLAWT_LRG_AGENT_FIELD_SWITCH:
        return g_strdup(clawt_lrg_switch_active(view->control) ? "true"
                                                               : "false");

    case CLAWT_LRG_AGENT_FIELD_ENUM:
    case CLAWT_LRG_AGENT_FIELD_TEAM:
    case CLAWT_LRG_AGENT_FIELD_ACCOUNT:
    case CLAWT_LRG_AGENT_FIELD_SHARED:
    case CLAWT_LRG_AGENT_FIELD_PROVIDER:
    case CLAWT_LRG_AGENT_FIELD_MODEL:
    case CLAWT_LRG_AGENT_FIELD_EFFORT:
    case CLAWT_LRG_AGENT_FIELD_COMPUTER_TYPE:
    case CLAWT_LRG_AGENT_FIELD_IMAGE:
    case CLAWT_LRG_AGENT_FIELD_DISK:
    case CLAWT_LRG_AGENT_FIELD_RESOLUTION:
        return clawt_lrg_agent_choice_value(
            clawt_lrg_combo_id(view->control),
            view->other != NULL ? clawt_lrg_entry_text(view->other) : NULL);
    }

    return NULL;
}

/* One running save: the plan, where it is, and what it learned. */
typedef struct {
    GPtrArray *plan;
    guint      next;
    guint      sent;
    gboolean   restart;
    gchar     *agent_id;
} SaveJob;

static void
save_job_free(SaveJob *job)
{
    g_ptr_array_unref(job->plan);
    g_free(job->agent_id);
    g_free(job);
}

static void save_next(ClawtLrgApp *app, SaveJob *job);

static void
on_saved_one(ClawtLrgApp *app, JsonNode *reply, const GError *error,
             gpointer user_data)
{
    SaveJob *job = user_data;

    /*
     * Stops at the first refusal, which is already toasted: the rest of
     * the plan was ordered on the assumption this one landed.  The page
     * is re-read so it shows what the daemon actually has.
     */
    if (reply == NULL) {
        if (ins.save != NULL)
            lrg_widget_set_enabled(ins.save, TRUE);
        save_job_free(job);
        refresh(app);
        return;
    }

    if (clawt_ipc_payload_boolean(clawt_ipc_reply_object(reply),
                                  "restart_required", FALSE))
        job->restart = TRUE;

    job->sent++;
    save_next(app, job);
}

static void
save_next(ClawtLrgApp *app, SaveJob *job)
{
    ClawtLrgAgentSetting *setting;

    if (job->next >= job->plan->len) {
        g_autofree gchar *said =
            clawt_lrg_agent_saved_text(job->sent, job->restart);

        /*
         * Said plainly: most of these take effect at the next start, and
         * a reload does not restart running agents on purpose.
         */
        clawt_lrg_toast(app, said);
        if (ins.save != NULL)
            lrg_widget_set_enabled(ins.save, TRUE);
        save_job_free(job);
        clawt_lrg_refresh_fleet(app);
        refresh(app);
        return;
    }

    setting = g_ptr_array_index(job->plan, job->next++);

    /* Not guarded: a save somebody started finishes wherever they went. */
    clawt_lrg_request(app, "agent.set",
                      clawt_ipc_payload_new("agent", job->agent_id,
                                            "key", setting->key,
                                            "value", setting->value, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_saved_one, job, NULL);
}

static void
on_save(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    g_autoptr(GPtrArray) posted =
        g_ptr_array_new_with_free_func(clawt_lrg_agent_setting_free);
    SaveJob *job;
    guint i;

    if (app->agent_id == NULL || ins.views == NULL)
        return;

    for (i = 0; i < ins.views->len; i++) {
        FieldView *view = g_ptr_array_index(ins.views, i);
        g_autofree gchar *value = view_value(view);

        g_ptr_array_add(posted, clawt_lrg_agent_setting_new(view->field->key,
                                                      value));
    }

    /*
     * The fallback list, spelled by the library.  An entry with no model
     * stops the save before anything is sent, rather than halfway.
     */
    if (ins.fallbacks != NULL && ins.fallbacks_saved != NULL) {
        g_autoptr(GPtrArray) list = read_fallbacks();
        g_autoptr(GError) why = NULL;
        g_autofree gchar *spelled = clawt_lrg_agent_fallback_spell(list, &why);

        if (spelled == NULL) {
            clawt_lrg_toast(app, why->message);
            return;
        }

        g_ptr_array_add(posted, clawt_lrg_agent_setting_new("model.fallbacks",
                                                            spelled));
    }

    job = g_new0(SaveJob, 1);
    job->plan = clawt_lrg_agent_save_plan(posted,
                                          shown_object("settings"));
    job->agent_id = g_strdup(app->agent_id);

    lrg_widget_set_enabled(ins.save, FALSE);
    save_next(app, job);
}

static void
field_view_free(gpointer data)
{
    g_free(data);
}

static guint
build_form(ClawtLrgApp *app, guint index)
{
    const gchar *use = clawt_ipc_payload_string(shown_object("settings"),
                                                "computer.use");
    guint g;
    guint f;

    g_clear_pointer(&ins.views, g_ptr_array_unref);
    g_clear_pointer(&ins.form, g_ptr_array_unref);

    ins.form = clawt_lrg_agent_form(shown_object("settings"));
    ins.views = g_ptr_array_new_with_free_func(field_view_free);

    for (g = 0; g < ins.form->len; g++) {
        ClawtLrgAgentGroup *group = g_ptr_array_index(ins.form, g);
        LrgWidget *box = section_card(app, ins.body, group->title,
                                      group->summary, index++);
        LrgWidget *form = clawt_lrg_form_new();

        for (f = 0; f < group->fields->len; f++) {
            ClawtLrgAgentField *field = g_ptr_array_index(group->fields, f);
            FieldView *view = g_new0(FieldView, 1);
            LrgWidget *outer = clawt_lrg_vbox(4.0f);

            /*
             * The control lives in a slot of its own, because a chooser
             * is redrawn into it when the daemon answers; anything said
             * beside it goes in the box around, which nothing clears.
             */
            view->field = field;
            view->slot = clawt_lrg_vbox(4.0f);
            clawt_lrg_add(outer, view->slot);
            g_object_unref(view->slot);
            build_control(view);
            g_ptr_array_add(ins.views, view);

            /*
             * An option that hands over real authority says so where it
             * is set, not only in the documentation.
             */
            if (field->dangerous)
                clawt_lrg_take(outer, clawt_lrg_badge(
                    "hands over real authority -- read what it does first",
                    "warn"));

            /*
             * And when the sandbox cannot be given here -- no
             * bubblewrap, or an embedded runtime -- the reason the agent
             * is a shadow, beside the setting that caused it, in the
             * library's words so the three clients agree with the
             * daemon.
             */
            if (g_strcmp0(field->key, "runtime.confine") == 0) {
                JsonObject *agent = shown_object("agent");
                const gchar *notice = clawt_agent_confine_notice(
                    clawt_ipc_payload_string(agent, "confine"),
                    clawt_ipc_payload_string(agent, "runtime_type"),
                    clawt_ipc_payload_boolean(agent, "confine_available",
                                              TRUE));

                if (notice != NULL) {
                    clawt_lrg_take(outer, clawt_lrg_badge(
                        "cannot be sandboxed here", "warn"));
                    clawt_lrg_take(outer, clawt_lrg_paragraph(notice,
                                                              "caption"));
                }
            }

            /*
             * The network can be chosen only inside the sandbox; outside
             * it the control is greyed with the library's reason, unless
             * a network is already set -- then the agent is a shadow and
             * the control stays live so it can be opened again.
             */
            if (g_strcmp0(field->key, "runtime.network") == 0) {
                JsonObject *agent = shown_object("agent");
                const gchar *confine =
                    clawt_ipc_payload_string(agent, "confine");
                const gchar *network =
                    clawt_ipc_payload_string(agent, "network");
                const gchar *notice =
                    clawt_egress_network_notice(network, confine);
                gboolean gated = network != NULL &&
                                 g_strcmp0(network, "open") != 0;

                if (notice != NULL) {
                    if (!gated && view->control != NULL)
                        lrg_widget_set_enabled(view->control, FALSE);

                    clawt_lrg_take(outer, clawt_lrg_badge(
                        gated ? "cannot be gated here"
                              : "needs the sandbox", "warn"));
                    clawt_lrg_take(outer, clawt_lrg_paragraph(notice,
                                                              "caption"));
                }
            }

            if (field->kind == CLAWT_LRG_AGENT_FIELD_SHARED && use != NULL &&
                *use != '\0')
                clawt_lrg_take(outer, clawt_lrg_paragraph(
                    "This agent is on a shared computer, so the rest of its "
                    "computer settings are that machine's, under computers:. "
                    "Choose \"Its own machine\" and save to give it one of "
                    "its own.", "caption"));

            clawt_lrg_form_add(form,
                               field->kind == CLAWT_LRG_AGENT_FIELD_SWITCH
                                   ? NULL : field->label,
                               outer, field->hint);
        }

        clawt_lrg_take(box, form);

        /* Right under the model it falls back from. */
        if (g_strcmp0(group->prefix, "model") == 0)
            index = build_fallbacks(app, index);
    }

    {
        LrgWidget *box = clawt_lrg_hbox(12.0f);
        LrgWidget *card;

        ins.save = clawt_lrg_button("Save changes", "primary",
                                    G_CALLBACK(on_save), app);
        clawt_lrg_add(box, ins.save);
        g_object_unref(ins.save);
        {
            LrgWidget *note = clawt_lrg_paragraph(
                "Only what you changed is sent. The model and the computer "
                "take effect when the agent next starts.", "muted");

            /* beside a button, the paragraph is what gives way */
            lrg_widget_set_fill_weight(note, 1.0f);
            clawt_lrg_take(box, note);
        }

        card = clawt_lrg_card(box);
        clawt_lrg_appear(app, card, index++);
        clawt_lrg_take(ins.body, card);
    }

    ask_for_choices(app);
    return index;
}

/* ── Credentials and the computer's own description ───────────────── */

static guint
build_credentials(ClawtLrgApp *app, JsonObject *agent, guint index)
{
    JsonObject *credentials;
    g_autoptr(GList) names = NULL;
    LrgWidget *box;
    GList *l;

    if (agent == NULL || !json_object_has_member(agent, "credentials"))
        return index;

    credentials = json_object_get_object_member(agent, "credentials");
    names = (credentials != NULL) ? json_object_get_members(credentials)
                                  : NULL;

    if (names == NULL)
        return index;

    /* Named, never valued: nothing secret can end up in a screenshot. */
    box = section_card(app, ins.body, "Credentials",
                       "Shown by reference. clawtilla never sends the values "
                       "to a client.", index++);

    for (l = names; l != NULL; l = l->next)
        clawt_lrg_take(box, info_row(l->data, clawt_ipc_payload_string(
            credentials, l->data)));

    return index;
}

/*
 * Where the agent may connect, what the gate refused it today, and which
 * of its keys the broker holds -- in the library's words, so the three
 * clients say "egress: gated (3 refused today)" alike.
 */
static guint
build_egress(ClawtLrgApp *app, guint index)
{
    JsonObject *root = shown_root();
    JsonObject *egress;
    JsonArray *refusals;
    JsonArray *keys;
    g_autofree gchar *summary = NULL;
    LrgWidget *box;
    guint i;

    if (root == NULL || !json_object_has_member(root, "egress"))
        return index;

    egress = json_object_get_object_member(root, "egress");

    if (egress == NULL)
        return index;

    summary = clawt_egress_status_line(
        clawt_ipc_payload_string(egress, "network"),
        (guint)clawt_ipc_payload_int(egress, "refused_today", 0));
    box = section_card(app, ins.body, clawt_egress_network_label(), summary,
                       index++);

    refusals = json_object_has_member(egress, "refusals")
               ? json_object_get_array_member(egress, "refusals") : NULL;

    for (i = 0; refusals != NULL && i < json_array_get_length(refusals);
         i++) {
        JsonObject *refusal = json_array_get_object_element(refusals, i);
        g_autofree gchar *line = NULL;

        if (refusal == NULL)
            continue;

        line = clawt_egress_refusal_line(
            clawt_ipc_payload_string(refusal, "host"),
            (guint)clawt_ipc_payload_int(refusal, "port", 0),
            clawt_ipc_payload_string(refusal, "address"),
            clawt_ipc_payload_string(refusal, "reason"));
        clawt_lrg_take(box, clawt_lrg_paragraph(line, "caption"));
    }

    keys = json_object_has_member(egress, "keys")
           ? json_object_get_array_member(egress, "keys") : NULL;

    for (i = 0; keys != NULL && i < json_array_get_length(keys); i++) {
        JsonObject *key = json_array_get_object_element(keys, i);
        gboolean brokered;

        if (key == NULL)
            continue;

        brokered = clawt_ipc_payload_boolean(key, "brokered", FALSE);
        clawt_lrg_take(box, info_row(
            clawt_ipc_payload_string(key, "name"),
            brokered ? "brokered" : "not brokered"));

        if (!brokered && clawt_ipc_payload_string(key, "reason") != NULL)
            clawt_lrg_take(box, clawt_lrg_paragraph(
                clawt_ipc_payload_string(key, "reason"), "caption"));
    }

    return index;
}

static void
on_open_computer(LrgButton *button, gpointer user_data)
{
    clawt_lrg_show_page(user_data, CLAWT_PAGE_COMPUTER);
}

static guint
build_computer_detail(ClawtLrgApp *app, JsonObject *agent, guint index)
{
    const gchar *detail = clawt_ipc_payload_string(shown_root(),
                                                   "computer_detail");
    const gchar *type = clawt_ipc_payload_string(agent, "computer");
    LrgWidget *box;
    LrgWidget *text;

    if (detail == NULL && (type == NULL || g_strcmp0(type, "none") == 0))
        return index;

    box = section_card(app, ins.body, "Computer",
                       "What the agent itself is told about its computer -- "
                       "the text that decides whether it goes looking for a "
                       "tool it does not have.", index++);

    if (detail != NULL) {
        text = clawt_lrg_editor(detail, NULL);
        lrg_text_editor_set_read_only(LRG_TEXT_EDITOR(text), TRUE);
        lrg_widget_add_class(text, "clawt-terminal");
        clawt_lrg_take(box, text);
    }

    /*
     * The machine's verbs -- start, stop, restart and the rebuild that
     * applies what cloud-init reads once -- live on the Computer page,
     * with the confirmation each needs, so there is one of each.
     */
    if (type != NULL && g_strcmp0(type, "none") != 0)
        clawt_lrg_take(box, clawt_lrg_button(
            "Machine, shell and rebuild \xe2\x86\x92", "flat",
            G_CALLBACK(on_open_computer), app));

    return index;
}

/* ── Integrations ─────────────────────────────────────────────────── */

typedef struct {
    gchar    *name;
    gboolean  wanted;
    gchar    *agent_id;
} IntegrationToggle;

static void
integration_toggle_free(gpointer data)
{
    IntegrationToggle *toggle = data;

    g_free(toggle->name);
    g_free(toggle->agent_id);
    g_free(toggle);
}

static void ask_integrations(ClawtLrgApp *app);

static void
on_integration_updated(ClawtLrgApp *app, JsonNode *reply, const GError *error,
                       gpointer user_data)
{
    ask_integrations(app);
}

/*
 * The switch edits the instance's `agents` list rather than writing into
 * the agent, because that is where the scope lives.  Read fresh first:
 * the list somebody else changed a moment ago is the one to edit.
 */
static void
on_integration_list(ClawtLrgApp *app, JsonNode *reply, const GError *error,
                    gpointer user_data)
{
    IntegrationToggle *toggle = user_data;
    g_autofree gchar *refusal = NULL;
    JsonNode *payload;

    if (reply == NULL) {
        ask_integrations(app);
        return;
    }

    payload = clawt_lrg_agent_integration_update(reply, toggle->name,
                                                 toggle->agent_id,
                                                 toggle->wanted, &refusal);

    if (payload == NULL) {
        clawt_lrg_toast(app, refusal);
        ask_integrations(app);
        return;
    }

    clawt_lrg_request(app, "integration.update", payload,
                      CLAWT_LRG_REQUEST_NONE, on_integration_updated, NULL,
                      NULL);
}

static void
on_integration_toggled(LrgCheckbox *box, gboolean active, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    IntegrationToggle *toggle;

    if (ins.toggling || app->agent_id == NULL)
        return;

    toggle = g_new0(IntegrationToggle, 1);
    toggle->name = g_strdup(g_object_get_data(G_OBJECT(box),
                                              "clawt-integration"));
    toggle->wanted = active;
    toggle->agent_id = g_strdup(app->agent_id);

    clawt_lrg_request(app, "integration.list", NULL, CLAWT_LRG_REQUEST_NONE,
                      on_integration_list, toggle, integration_toggle_free);
}

static void
on_integrations(ClawtLrgApp *app, JsonNode *reply, const GError *error,
                gpointer user_data)
{
    g_autoptr(GPtrArray) rows = clawt_lrg_agent_integration_rows(reply);
    guint i;

    if (ins.integrations == NULL)
        return;

    clawt_lrg_clear(ins.integrations);

    if (rows->len == 0) {
        clawt_lrg_take(ins.integrations, clawt_lrg_paragraph(
            reply == NULL
                ? "The integrations could not be read."
                : "None yet. Settings \xe2\x86\x92 Integrations adds one -- a "
                  "Matrix account to talk to this agent from your phone, or "
                  "an MCP server to give it tools.", "muted"));
        clawt_lrg_app_wake(app);
        return;
    }

    ins.toggling = TRUE;

    for (i = 0; i < rows->len; i++) {
        ClawtLrgAgentIntegrationRow *row = g_ptr_array_index(rows, i);
        LrgWidget *line = clawt_lrg_row(row->name, row->subtitle);

        if (row->shared) {
            LrgWidget *toggle = clawt_lrg_switch("", row->covers);

            g_object_set_data_full(G_OBJECT(toggle), "clawt-integration",
                                   g_strdup(row->name), g_free);

            /* Connected after the state is set: building is not a flip. */
            g_signal_connect(toggle, "toggled",
                             G_CALLBACK(on_integration_toggled), app);
            clawt_lrg_row_add_suffix(line, toggle);
        } else {
            clawt_lrg_row_add_suffix(line, clawt_lrg_badge("its own",
                                                           "neutral"));
        }

        clawt_lrg_appear(app, line, i);
        clawt_lrg_take(ins.integrations, line);
    }

    ins.toggling = FALSE;
    clawt_lrg_app_wake(app);
}

static void
ask_integrations(ClawtLrgApp *app)
{
    if (app->agent_id == NULL)
        return;

    clawt_lrg_request(app, "integration.list",
                      clawt_ipc_payload_new("agent", app->agent_id, NULL),
                      CLAWT_LRG_GUARDED, on_integrations, NULL, NULL);
}

static guint
build_integrations(ClawtLrgApp *app, guint index)
{
    LrgWidget *box = section_card(
        app, ins.body, "Integrations",
        "What reaches this agent from outside the fleet. Add and configure "
        "them in Settings; here you choose which ones this agent gets.",
        index++);

    ins.integrations = clawt_lrg_vbox(6.0f);
    clawt_lrg_add(box, ins.integrations);
    g_object_unref(ins.integrations);

    ask_integrations(app);
    return index;
}

/* ── Shared folders ───────────────────────────────────────────────── */

static void ask_mounts(ClawtLrgApp *app);

static void
on_mount_changed(ClawtLrgApp *app, JsonNode *reply, const GError *error,
                 gpointer user_data)
{
    if (reply != NULL)
        clawt_lrg_toast(app, user_data);

    ask_mounts(app);
}

static void
on_mount_remove(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    const gchar *target = g_object_get_data(G_OBJECT(button), "clawt-target");

    if (app->agent_id == NULL || target == NULL)
        return;

    clawt_lrg_request(app, "agent.mount.remove",
                      clawt_ipc_payload_new("agent", app->agent_id,
                                            "target", target, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_mount_changed,
                      (gpointer)"Removed. Takes effect on the next restart.",
                      NULL);
}

/*
 * Shares a host folder with the agent's computer.  Relabel goes on the
 * frame too: absent means `shared`, which is podman relabelling every
 * file at every start -- refused on a folder this user does not own, by
 * an error naming a setting a client must be able to reach.
 */
static void
on_mount_add(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    const gchar *source;
    const gchar *target;
    const gchar *refusal;

    if (app->agent_id == NULL || ins.mount_source == NULL)
        return;

    source = clawt_lrg_entry_text(ins.mount_source);
    target = clawt_lrg_entry_text(ins.mount_target);
    refusal = clawt_lrg_agent_mount_refusal(source, target);

    if (refusal != NULL) {
        clawt_lrg_toast(app, refusal);
        return;
    }

    clawt_lrg_request(app, "agent.mount.add",
                      clawt_ipc_payload_new(
                          "agent", app->agent_id, "source", source,
                          "target", target,
                          "mode", clawt_lrg_combo_id(ins.mount_mode),
                          "relabel", clawt_lrg_combo_id(ins.mount_relabel),
                          NULL),
                      CLAWT_LRG_REQUEST_NONE, on_mount_changed,
                      (gpointer)"Shared. It appears when the agent restarts.",
                      NULL);
}

static void
on_mounts(ClawtLrgApp *app, JsonNode *reply, const GError *error,
          gpointer user_data)
{
    JsonObject *root = clawt_ipc_reply_object(reply);
    JsonArray *mounts = (root != NULL && json_object_has_member(root,
                                                                "mounts"))
        ? json_object_get_array_member(root, "mounts") : NULL;
    guint i;

    if (ins.mounts == NULL)
        return;

    clawt_lrg_clear(ins.mounts);

    for (i = 0; mounts != NULL && i < json_array_get_length(mounts); i++) {
        JsonObject *mount = json_array_get_object_element(mounts, i);
        const gchar *target = clawt_ipc_payload_string(mount, "target");
        g_autofree gchar *subtitle = clawt_lrg_agent_mount_subtitle(mount);
        LrgWidget *row = clawt_lrg_row(target != NULL ? target : "?",
                                       subtitle);
        LrgWidget *remove = clawt_lrg_button("Stop sharing", "flat",
                                             G_CALLBACK(on_mount_remove),
                                             app);

        g_object_set_data_full(G_OBJECT(remove), "clawt-target",
                               g_strdup(target), g_free);
        clawt_lrg_row_add_suffix(row, remove);
        clawt_lrg_appear(app, row, i);
        clawt_lrg_take(ins.mounts, row);
    }

    clawt_lrg_app_wake(app);
}

static void
ask_mounts(ClawtLrgApp *app)
{
    if (app->agent_id == NULL || ins.mounts == NULL)
        return;

    clawt_lrg_request(app, "agent.mount.list",
                      clawt_ipc_payload_new("agent", app->agent_id, NULL),
                      CLAWT_LRG_GUARDED, on_mounts, NULL, NULL);
}

/*
 * For a computer that can have them.  A host agent is left out: its
 * computer is this machine, and what limits it is confinement, not a
 * mount list.  A shared machine's mounts are the machine's.
 */
static guint
build_mounts(ClawtLrgApp *app, JsonObject *agent, guint index)
{
    const gchar *type = clawt_ipc_payload_string(agent, "computer");
    const gchar *use = clawt_ipc_payload_string(shown_object("settings"),
                                                "computer.use");
    gint value = CLAWT_COMPUTER_NONE;
    g_autoptr(ClawtLrgAgentChoices) modes = NULL;
    g_autoptr(ClawtLrgAgentChoices) relabels = NULL;
    LrgWidget *box;
    LrgWidget *form;

    ins.mounts = NULL;
    ins.mount_source = NULL;

    clawt_enum_from_nick(CLAWT_TYPE_COMPUTER_TYPE, type, &value);

    if (!clawt_computer_type_takes_mounts((ClawtComputerType)value) ||
        (use != NULL && *use != '\0'))
        return index;

    box = section_card(app, ins.body, "Shared folders",
                       clawt_lrg_agent_mounts_summary(type), index++);

    ins.mounts = clawt_lrg_vbox(6.0f);
    clawt_lrg_add(box, ins.mounts);
    g_object_unref(ins.mounts);

    form = clawt_lrg_form_new();

    ins.mount_source = clawt_lrg_entry(NULL, "/home/you/project");
    clawt_lrg_form_add(form, "Folder on this machine",
                       g_object_ref(ins.mount_source), NULL);
    g_object_unref(ins.mount_source);

    ins.mount_target = clawt_lrg_entry(NULL, "/project");
    clawt_lrg_form_add(form, "Where the agent sees it",
                       g_object_ref(ins.mount_target), NULL);
    g_object_unref(ins.mount_target);

    modes = clawt_lrg_agent_mount_mode_choices();
    ins.mount_mode = clawt_lrg_combo(clawt_lrg_agent_choices_ids(modes),
                                     clawt_lrg_agent_choices_labels(modes),
                                     modes->selected);
    clawt_lrg_form_add(form, "Access", g_object_ref(ins.mount_mode), NULL);
    g_object_unref(ins.mount_mode);

    relabels = clawt_lrg_agent_relabel_choices();
    ins.mount_relabel = clawt_lrg_combo(clawt_lrg_agent_choices_ids(relabels),
                                        clawt_lrg_agent_choices_labels(relabels),
                                        relabels->selected);
    clawt_lrg_form_add(form, "SELinux relabel",
                       g_object_ref(ins.mount_relabel),
                       "Relabelling walks every file under the folder at each "
                       "start. Pick none for a folder you do not own.");
    g_object_unref(ins.mount_relabel);

    clawt_lrg_take(form, clawt_lrg_button("Share this folder", "primary",
                                          G_CALLBACK(on_mount_add), app));
    clawt_lrg_take(box, form);

    ask_mounts(app);
    return index;
}

/* ── Waiting for ──────────────────────────────────────────────────── */

static void ask_waits(ClawtLrgApp *app);

static void
on_wait_cancelled(
	ClawtLrgApp  *app,
	JsonNode     *reply,
	const GError *error,
	gpointer      user_data
){
	if (reply != NULL)
		clawt_lrg_toast(app, "Cancelled. The agent has been told it is no "
		                "longer waiting.");

	ask_waits(app);
}

static void
on_wait_cancel(
	LrgButton *button,
	gpointer   user_data
){
	ClawtLrgApp *app = user_data;
	const gchar *id = g_object_get_data(G_OBJECT(button), "clawt-wait");

	if (id == NULL)
		return;

	clawt_lrg_request(app, "wait.cancel",
	                  clawt_ipc_payload_new("wait", id, NULL),
	                  CLAWT_LRG_REQUEST_NONE, on_wait_cancelled, NULL, NULL);
}

/*
 * One row per pending wait.  The schedule line is
 * clawt_wait_schedule_label()'s, so it reads as the GTK and web clients
 * draw it.
 */
static void
on_waits(
	ClawtLrgApp  *app,
	JsonNode     *reply,
	const GError *error,
	gpointer      user_data
){
	JsonObject *root = clawt_ipc_reply_object(reply);
	JsonArray *waits = (root != NULL && json_object_has_member(root, "waits"))
		? json_object_get_array_member(root, "waits") : NULL;
	gint64 now = g_get_real_time();
	guint i;

	if (ins.waits == NULL)
		return;

	clawt_lrg_clear(ins.waits);

	if (waits == NULL || json_array_get_length(waits) == 0)
		clawt_lrg_take(ins.waits, clawt_lrg_paragraph(
			"Nothing. When this agent sets a wait with "
			"clawtilla_wait_until, it shows here until it ends.", "muted"));

	for (i = 0; waits != NULL && i < json_array_get_length(waits); i++) {
		JsonObject *wait = json_array_get_object_element(waits, i);
		const gchar *label = clawt_ipc_payload_string(wait, "label");
		const gchar *condition = clawt_ipc_payload_string(wait, "condition");
		const gchar *seen = clawt_ipc_payload_string(wait, "last_result");
		g_autofree gchar *schedule = clawt_wait_schedule_label(
			json_object_has_member(wait, "last_check_at")
				? json_object_get_int_member(wait, "last_check_at") : 0,
			json_object_has_member(wait, "next_check_at")
				? json_object_get_int_member(wait, "next_check_at") : 0,
			json_object_has_member(wait, "expires_at")
				? json_object_get_int_member(wait, "expires_at") : 0,
			now);
		g_autofree gchar *subtitle = g_strdup_printf(
			"%s\n%s%s%s", condition != NULL ? condition : "?", schedule,
			seen != NULL ? "\nLast check saw: " : "",
			seen != NULL ? seen : "");
		LrgWidget *row = clawt_lrg_row(label != NULL ? label : "?",
		                               subtitle);
		LrgWidget *cancel = clawt_lrg_button("Cancel", "flat",
		                                     G_CALLBACK(on_wait_cancel),
		                                     app);

		g_object_set_data_full(G_OBJECT(cancel), "clawt-wait",
		                       g_strdup(clawt_ipc_payload_string(wait, "id")),
		                       g_free);
		clawt_lrg_row_add_suffix(row, cancel);
		clawt_lrg_appear(app, row, i);
		clawt_lrg_take(ins.waits, row);
	}

	clawt_lrg_app_wake(app);
}

static void
ask_waits(
	ClawtLrgApp *app
){
	if (app->agent_id == NULL || ins.waits == NULL)
		return;

	clawt_lrg_request(app, "wait.list",
	                  clawt_ipc_payload_new("agent", app->agent_id, NULL),
	                  CLAWT_LRG_GUARDED | CLAWT_LRG_QUIET, on_waits, NULL,
	                  NULL);
}

/*
 * What this agent is waiting for: conditions clawtilla checks by code on
 * its behalf.  Shown because a wait is work happening while nothing
 * appears to be -- an agent that looks idle and is waiting on a deploy
 * is not one somebody should nudge.
 */
static guint
build_waits(
	ClawtLrgApp *app,
	guint        index
){
	LrgWidget *box = section_card(
		app, ins.body, "Waiting for",
		"Conditions clawtilla checks for this agent, by code -- a pending "
		"wait costs nothing. The agent gets one notice when each holds or "
		"expires; cancelling one tells it.", index++);

	ins.waits = clawt_lrg_vbox(6.0f);
	clawt_lrg_add(box, ins.waits);
	g_object_unref(ins.waits);

	ask_waits(app);
	return index;
}

/* ── Files ────────────────────────────────────────────────────────── */

static void
on_open_file(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    const gchar *path = g_object_get_data(G_OBJECT(button), "clawt-path");
    const gchar *name = g_object_get_data(G_OBJECT(button), "clawt-name");

    if (path == NULL || *path == '\0')
        return;

    /*
     * A path on the daemon's machine.  Over a connection to another one
     * it does not exist here, and the CLI's `agent edit` is the route.
     */
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_autofree gchar *said = g_strdup_printf(
            "%s is not on this machine; try `clawtilla agent edit %s %s`",
            path, app->agent_id != NULL ? app->agent_id : "<agent>",
            name != NULL ? name : "<file>");

        clawt_lrg_toast(app, said);
        return;
    }

    clawt_lrg_open_in_editor(app, path);
}

static void
on_files(ClawtLrgApp *app, JsonNode *reply, const GError *error,
         gpointer user_data)
{
    JsonObject *root = clawt_ipc_reply_object(reply);
    JsonArray *files = (root != NULL && json_object_has_member(root, "files"))
        ? json_object_get_array_member(root, "files") : NULL;
    guint i;

    if (ins.files == NULL)
        return;

    clawt_lrg_clear(ins.files);

    for (i = 0; files != NULL && i < json_array_get_length(files); i++) {
        JsonObject *file = json_array_get_object_element(files, i);
        const gchar *name = clawt_ipc_payload_string(file, "name");
        g_autofree gchar *subtitle = clawt_lrg_agent_file_subtitle(file);
        LrgWidget *row = clawt_lrg_row(name != NULL ? name : "?", subtitle);
        LrgWidget *open = clawt_lrg_button("Open in $EDITOR", "flat",
                                           G_CALLBACK(on_open_file), app);

        g_object_set_data_full(G_OBJECT(open), "clawt-path",
                               g_strdup(clawt_ipc_payload_string(file, "path")),
                               g_free);
        g_object_set_data_full(G_OBJECT(open), "clawt-name", g_strdup(name),
                               g_free);
        clawt_lrg_row_add_suffix(row, open);
        clawt_lrg_appear(app, row, i);
        clawt_lrg_take(ins.files, row);
    }

    clawt_lrg_app_wake(app);
}

/*
 * The files the agent reads as its system prompt, and the .mcp.json that
 * decides which servers it may call -- where an agent is actually
 * configured, as opposed to merely wired up.
 */
static guint
build_files(ClawtLrgApp *app, guint index)
{
    LrgWidget *box = section_card(
        app, ins.body, "Files",
        "Opened in $EDITOR. The ones marked \"prompt\" are read into every "
        "turn; .mcp.json is the list of MCP servers this agent may call, and "
        "clawtilla only ever rewrites its own entries in it.", index++);

    ins.files = clawt_lrg_vbox(6.0f);
    clawt_lrg_add(box, ins.files);
    g_object_unref(ins.files);

    clawt_lrg_request(app, "agent.files",
                      clawt_ipc_payload_new("agent", app->agent_id, NULL),
                      CLAWT_LRG_GUARDED, on_files, NULL, NULL);
    return index;
}

/* ── Resetting and removing ───────────────────────────────────────── */

static void
on_reset_done(ClawtLrgApp *app, JsonNode *reply, const GError *error,
              gpointer user_data)
{
    if (reply != NULL) {
        g_autofree gchar *said = clawt_lrg_agent_reset_text(reply);

        clawt_lrg_toast(app, said);
    }

    clawt_lrg_refresh_fleet(app);
}

static void
on_reset_confirmed(ClawtLrgApp *app, gpointer user_data)
{
    if (app->agent_id == NULL)
        return;

    clawt_lrg_request(app, "agent.reset",
                      clawt_ipc_payload_new("agent", app->agent_id, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_reset_done, NULL, NULL);
}

/*
 * A fresh session, which is what an identity change needs: an AI CLI is
 * not handed a system prompt when it resumes, so a restart alone keeps
 * the old persona.
 */
static void
on_reset(LrgButton *button, gpointer user_data)
{
    clawt_lrg_confirm(
        user_data, "Start a fresh session?",
        "The agent forgets the conversation it is in; the next thing you say "
        "starts a new one. Its stored sessions are moved aside rather than "
        "deleted, and a running agent is restarted.",
        "Reset", TRUE, on_reset_confirmed, NULL, NULL);
}

typedef struct {
    gchar     *agent_id;
    LrgWidget *remove_computer;   /* nullable */
    LrgWidget *remove_files;
} RemoveAsk;

static void
remove_ask_free(gpointer data)
{
    RemoveAsk *ask = data;

    g_free(ask->agent_id);
    g_clear_object(&ask->remove_computer);
    g_clear_object(&ask->remove_files);
    g_free(ask);
}

static void
on_removed(ClawtLrgApp *app, JsonNode *reply, const GError *error,
           gpointer user_data)
{
    const gchar *agent_id = user_data;
    g_autofree gchar *said = NULL;

    if (reply == NULL)
        return;

    said = clawt_lrg_agent_removed_text(agent_id, reply);
    clawt_lrg_toast(app, said);

    if (g_strcmp0(app->agent_id, agent_id) == 0)
        clawt_lrg_select_agent(app, NULL);

    clawt_lrg_refresh_fleet(app);
}

static gboolean
on_remove_confirmed_twice(ClawtLrgApp *app, LrgDialog *dialog, gint response,
                          gpointer user_data)
{
    RemoveAsk *ask = user_data;
    gboolean with_computer = ask->remove_computer != NULL &&
                             clawt_lrg_switch_active(ask->remove_computer);
    gboolean purge = clawt_lrg_switch_active(ask->remove_files);

    /*
     * The checkboxes are read off this dialog, not the window: a second
     * delete started before the first finished must not read the wrong
     * ones.  The flags are strings the daemon's boolean reader accepts.
     */
    clawt_lrg_request(app, "agent.remove",
                      clawt_ipc_payload_new(
                          "agent", ask->agent_id,
                          "remove_computer", with_computer ? "true" : NULL,
                          "remove_files", purge ? "true" : NULL, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_removed,
                      g_strdup(ask->agent_id), g_free);

    return TRUE;
}

static void
on_remove_confirmed_once(ClawtLrgApp *app, gpointer user_data)
{
    JsonObject *agent = clawt_lrg_find_agent(app, user_data);
    const gchar *type = clawt_ipc_payload_string(agent, "computer");
    RemoveAsk *ask = g_new0(RemoveAsk, 1);
    LrgWidget *box = clawt_lrg_vbox(10.0f);

    ask->agent_id = g_strdup(user_data);

    /*
     * The machine is a separate decision, off by default: a container
     * may hold work that was never anywhere else, and removing the agent
     * used to leave it running under a name nobody remembered.
     */
    if (g_strcmp0(type, "container") == 0 || g_strcmp0(type, "vm") == 0) {
        g_autofree gchar *label = g_strdup_printf(
            "Also delete its %s, clawt-%s",
            g_strcmp0(type, "vm") == 0 ? "virtual machine" : "container",
            ask->agent_id);

        ask->remove_computer = clawt_lrg_switch(label, FALSE);
        clawt_lrg_add(box, ask->remove_computer);
    }

    /* And the files: the half with no undo, worded without euphemism. */
    ask->remove_files = clawt_lrg_switch(
        "Also delete everything it owns: its persona, notes, mailbox, "
        "memories, transcripts and credentials", FALSE);
    clawt_lrg_add(box, ask->remove_files);

    g_object_unref(clawt_lrg_dialog(app, "ARE YOU REALLY SURE!?",
                     "This removes the agent from the fleet and stops it if "
                     "it is running.\n\nIts mailbox and transcripts stay on "
                     "disk unless you say otherwise below -- removing an agent "
                     "is reversible, deleting its history is not.",
                     box, "Delete it for good", TRUE,
                     on_remove_confirmed_twice, ask, remove_ask_free));
    g_object_unref(box);
}

/*
 * Two steps, the second louder: removing an agent takes its process and
 * its place in every room, and one click between browsing and gone is
 * too few.
 */
static void
on_remove(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    g_autofree gchar *body = NULL;

    if (app->agent_id == NULL)
        return;

    body = g_strdup_printf("Delete %s?", app->agent_id);
    clawt_lrg_confirm(app, "Are you sure?", body, "Delete", TRUE,
                      on_remove_confirmed_once, g_strdup(app->agent_id),
                      g_free);
}

static guint
build_danger(ClawtLrgApp *app, guint index)
{
    LrgWidget *box = section_card(
        app, ins.body, "Danger zone",
        "Resetting starts a fresh session. Removing an agent stops it and "
        "takes it out of every room; its mailbox and transcripts stay on "
        "disk unless you choose otherwise.", index++);
    LrgWidget *buttons = clawt_lrg_hbox(8.0f);

    clawt_lrg_take(buttons, clawt_lrg_button("Reset session", NULL,
                                             G_CALLBACK(on_reset), app));
    clawt_lrg_take(buttons, clawt_lrg_button("Delete this agent", "danger",
                                             G_CALLBACK(on_remove), app));
    clawt_lrg_take(box, buttons);

    return index;
}

/* ── The page ─────────────────────────────────────────────────────── */

static void
draw_all(ClawtLrgApp *app)
{
    JsonObject *root = shown_root();
    JsonObject *agent = shown_object("agent");
    guint index = 0;
    LrgWidget *glance_card;

    clawt_lrg_clear(ins.body);
    ins.integrations = NULL;
    ins.mounts = NULL;
    ins.waits = NULL;
    ins.files = NULL;
    ins.save = NULL;
    ins.fallbacks = NULL;
    g_clear_pointer(&ins.fallback_views, g_ptr_array_unref);
    g_clear_pointer(&ins.accounts, json_node_unref);
    ins.usage_today = NULL;
    ins.usage_total = NULL;
    ins.mount_source = NULL;
    g_clear_pointer(&ins.catalog, json_node_unref);
    g_clear_pointer(&ins.images, json_node_unref);
    g_clear_pointer(&ins.disks, json_node_unref);

    if (root == NULL || agent == NULL) {
        clawt_lrg_take(ins.body, clawt_lrg_empty_state(
            "No such agent", "It may have been removed from the fleet."));
        clawt_lrg_app_wake(app);
        return;
    }

    clawt_lrg_page_set_subtitle(ins.page,
                                clawt_ipc_payload_string(agent, "description"));

    ins.glance = clawt_lrg_vbox(8.0f);
    glance_card = clawt_lrg_card(g_object_ref(ins.glance));
    g_object_unref(ins.glance);
    clawt_lrg_appear(app, glance_card, index++);
    clawt_lrg_take(ins.body, glance_card);
    draw_glance(app, ins.shown);

    build_avatar_group(app, agent, index++);
    draw_usage(app, index++);
    index = build_form(app, index);
    index = build_waits(app, index);
    index = build_credentials(app, agent, index);
    index = build_integrations(app, index);
    index = build_computer_detail(app, agent, index);
    index = build_egress(app, index);
    index = build_mounts(app, agent, index);
    index = build_files(app, index);
    build_danger(app, index);

    clawt_lrg_app_wake(app);
}

static void
on_shown(ClawtLrgApp *app, JsonNode *reply, const GError *error,
         gpointer user_data)
{
    if (reply == NULL) {
        clawt_lrg_clear(ins.body);
        clawt_lrg_take(ins.body, clawt_lrg_empty_state(
            "This agent cannot be read",
            error != NULL ? error->message : NULL));
        clawt_lrg_app_wake(app);
        return;
    }

    g_clear_pointer(&ins.shown, json_node_unref);
    ins.shown = json_node_copy(reply);
    draw_all(app);
}

/* An event redraws the glance only, never a form being edited. */
static void
on_glance(ClawtLrgApp *app, JsonNode *reply, const GError *error,
          gpointer user_data)
{
    if (reply == NULL || ins.glance == NULL || ins.shown == NULL)
        return;

    draw_glance(app, reply);
}

static void
refresh(ClawtLrgApp *app)
{
    if (ins.page == NULL)
        return;

    if (app->agent_id == NULL) {
        clawt_lrg_clear(ins.body);
        g_clear_pointer(&ins.shown, json_node_unref);
        g_clear_pointer(&ins.views, g_ptr_array_unref);
        g_clear_pointer(&ins.form, g_ptr_array_unref);
        g_clear_pointer(&ins.fallback_views, g_ptr_array_unref);
        ins.fallbacks = NULL;
        ins.glance = NULL;
        ins.save = NULL;
        ins.integrations = NULL;
        ins.mounts = NULL;
        ins.waits = NULL;
        ins.files = NULL;
        ins.usage_today = NULL;
        ins.usage_total = NULL;
        ins.mount_source = NULL;
        clawt_lrg_page_set_subtitle(ins.page, NULL);
        clawt_lrg_take(ins.body, clawt_lrg_empty_state(
            "No agent selected",
            "Pick an agent in the sidebar to see and change everything it "
            "is configured with."));
        clawt_lrg_app_wake(app);
        return;
    }

    clawt_lrg_request(app, "agent.show",
                      clawt_ipc_payload_new("agent", app->agent_id, NULL),
                      CLAWT_LRG_GUARDED, on_shown, NULL, NULL);
}

static void
event(ClawtLrgApp *app, ClawtEvent *ev)
{
    const gchar *kind = clawt_event_get_kind(ev);
    const gchar *subject = clawt_event_get_subject(ev);

    if (kind == NULL || app->page != CLAWT_PAGE_AGENT ||
        app->agent_id == NULL || ins.glance == NULL)
        return;

    /*
     * A typing frame is a level re-sent every 25 seconds; it changes
     * nothing drawn here.  Everything else about this agent, or the hold,
     * redraws the glance -- and only the glance, so an edit in progress
     * in the form is never thrown away by somebody else's event.
     */
    if (g_strcmp0(kind, "agent.typing") == 0)
        return;

    if (g_str_has_prefix(kind, "hold.") ||
        (g_str_has_prefix(kind, "agent.") &&
         (subject == NULL || *subject == '\0' ||
          g_strcmp0(subject, app->agent_id) == 0)))
        clawt_lrg_request(app, "agent.show",
                          clawt_ipc_payload_new("agent", app->agent_id, NULL),
                          CLAWT_LRG_GUARDED | CLAWT_LRG_QUIET, on_glance,
                          NULL, NULL);

    if (g_str_has_prefix(kind, "integration."))
        ask_integrations(app);

    /* Only the list is refilled, never the form somebody may be editing. */
    if (g_str_has_prefix(kind, "wait.") &&
        (subject == NULL || g_strcmp0(subject, app->agent_id) == 0))
        ask_waits(app);
}

static LrgWidget *
build(ClawtLrgApp *app)
{
    LrgWidget *page = clawt_lrg_page_new("Overview", NULL);

    memset(&ins, 0, sizeof(ins));
    ins.page = page;
    ins.body = clawt_lrg_page_body(page);

    return page;
}

static const ClawtLrgPage page_impl = {
    CLAWT_PAGE_AGENT, build, refresh, event
};

const ClawtLrgPage *
clawt_lrg_agent_page(void)
{
    return &page_impl;
}
