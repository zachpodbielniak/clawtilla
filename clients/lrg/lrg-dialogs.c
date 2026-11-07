/*
 * lrg-dialogs.c - New agent, Import an agent, New room
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The three things that add to the sidebar.  Mirrors, from
 * clients/gtk/clawt-window.c, the New agent dialog with its "Proposed
 * agent" design flow (clawt_gtk_on_new_agent) and the Import an agent
 * dialog (on_import_agent), and from clients/gtk/gtk-sidebar.c the New
 * room dialog (clawt_gtk_on_new_room).
 *
 * GTK fills each of these with blocking requests before it draws.
 * Here the dialog opens at once and each list -- teams, models, images,
 * cloud images, unclaimed workspaces -- arrives into a slot when its
 * reply does, so a slow `model.list` (it asks the providers) holds up
 * nothing but the model rows.
 *
 * Whatever is created is then selected: the fleet is re-read and the
 * new agent or room opened, because a dialog that closes onto a sidebar
 * still showing the old fleet reads as the create having failed.
 */

#include "clawt-lrg.h"
#include "clawt-lrg-settings-model.h"

/* The id a chooser gives its "type something else" entry. */
#define OTHER_ID "\001other"

/* ── Shared ───────────────────────────────────────────────────────── */

/* Text a person typed, or NULL when they left it blank. */
static const gchar *
answer(LrgWidget *entry)
{
    const gchar *text = clawt_lrg_entry_text(entry);

    return (text != NULL && *text != '\0') ? text : NULL;
}

/* The field box clawt_lrg_form_add() made around @control. */
static LrgWidget *
field_of(LrgWidget *control)
{
    return LRG_WIDGET(lrg_widget_get_parent(control));
}

/* A card with a heading, a paragraph and a body to fill. */
static LrgWidget *
group_card(const gchar *title, const gchar *description,
           LrgWidget **out_body)
{
    LrgWidget *box = clawt_lrg_vbox(10.0f);
    LrgWidget *body = clawt_lrg_vbox(10.0f);

    clawt_lrg_take(box, clawt_lrg_label(title, "heading"));

    if (description != NULL)
        clawt_lrg_take(box, clawt_lrg_paragraph(description, "muted"));

    clawt_lrg_add(box, body);
    *out_body = body;
    g_object_unref(body);

    return clawt_lrg_card(box);
}

/* A vertical scroller around @content, sized for a dialog. */
static LrgWidget *
scroller(ClawtLrgApp *app, LrgWidget *content, gfloat width, gfloat height)
{
    LrgScrollBox *scroll = lrg_scroll_box_new();

    lrg_scroll_box_set_hscroll(scroll, FALSE);
    lrg_scroll_box_set_vscroll(scroll, TRUE);
    lrg_scroll_box_set_kinetic(scroll, TRUE);
    lrg_scroll_box_set_child(scroll, content);
    lrg_widget_set_min_size(LRG_WIDGET(scroll), width * (gfloat)app->scale,
                            height * (gfloat)app->scale);

    return LRG_WIDGET(scroll);
}

/*
 * Re-reads the fleet and opens @agent_id once the new list has it.
 * The shell's own refresh replies into a function this file cannot
 * reach, and selecting before the list knows the agent would select an
 * agent with no direct room -- so this reads agent.list itself, the
 * way the shell does, and selects in the reply.
 */
static void
on_agents_for_select(ClawtLrgApp *app, JsonNode *reply, const GError *error,
                     gpointer user_data)
{
    if (reply != NULL) {
        g_clear_pointer(&app->agents, json_node_unref);
        app->agents = json_node_copy(reply);
    }

    clawt_lrg_select_agent(app, user_data);
    clawt_lrg_refresh_fleet(app);
}

static void
select_new_agent(ClawtLrgApp *app, const gchar *agent_id)
{
    if (agent_id == NULL) {
        clawt_lrg_refresh_fleet(app);
        return;
    }

    clawt_lrg_request(app, "agent.list", NULL, CLAWT_LRG_QUIET,
                      on_agents_for_select, g_strdup(agent_id), g_free);
}

/* ── Choosers ─────────────────────────────────────────────────────── */

/*
 * Provider, then model: the model list depends on the provider, and
 * offering every model flat is how "sonnet" gets offered for Ollama.
 */
typedef struct {
    JsonArray *providers;      /* owned; filtered by the model */
    LrgWidget *provider_slot;
    LrgWidget *model_slot;
    LrgWidget *provider;       /* combo, nullable until model.list */
    LrgWidget *model;          /* combo */
    LrgWidget *model_entry;
    LrgWidget *note;
} ModelChooser;

static JsonObject *
chooser_provider(ModelChooser *chooser)
{
    const gchar *id;
    guint i;

    if (chooser->provider == NULL || chooser->providers == NULL)
        return NULL;

    id = clawt_lrg_combo_id(chooser->provider);

    for (i = 0; i < json_array_get_length(chooser->providers); i++) {
        JsonObject *provider =
            json_array_get_object_element(chooser->providers, i);

        if (g_strcmp0(clawt_ipc_payload_string(provider, "id"), id) == 0)
            return provider;
    }

    return NULL;
}

static const gchar *
chooser_provider_id(ModelChooser *chooser)
{
    return clawt_ipc_payload_string(chooser_provider(chooser), "id");
}

/* The model chosen from the list, or typed; NULL for neither. */
static const gchar *
chooser_model(ModelChooser *chooser)
{
    const gchar *id;

    if (chooser->model == NULL)
        return NULL;

    id = clawt_lrg_combo_id(chooser->model);

    if (g_strcmp0(id, OTHER_ID) == 0)
        return answer(chooser->model_entry);

    return id;
}

/* The free-text row only while "Something else…" is chosen. */
static void
on_model_changed(LrgComboBox *combo, gpointer user_data)
{
    ModelChooser *chooser = user_data;

    lrg_widget_set_visible(field_of(chooser->model_entry),
                           g_strcmp0(clawt_lrg_combo_id(chooser->model),
                                     OTHER_ID) == 0);
}

static void
on_provider_changed(LrgComboBox *combo, gpointer user_data)
{
    ModelChooser *chooser = user_data;
    JsonObject *provider = chooser_provider(chooser);
    JsonArray *models = clawt_lrg_settings_member_array(provider, "models");
    g_autoptr(GPtrArray) ids = g_ptr_array_new();
    g_autoptr(GPtrArray) labels = g_ptr_array_new_with_free_func(g_free);
    const gchar *note = clawt_ipc_payload_string(provider, "note");
    guint i;

    for (i = 0; models != NULL && i < json_array_get_length(models); i++) {
        JsonObject *model = json_array_get_object_element(models, i);
        const gchar *id = clawt_ipc_payload_string(model, "id");

        if (id == NULL)
            continue;

        g_ptr_array_add(ids, (gpointer)id);
        g_ptr_array_add(labels, clawt_lrg_create_model_label(model));
    }

    /*
     * The catalogue is curated and goes stale, so every provider also
     * takes a name typed by hand, as the last entry.
     */
    g_ptr_array_add(ids, (gpointer)OTHER_ID);
    g_ptr_array_add(labels, g_strdup("Something else…"));
    g_ptr_array_add(ids, NULL);
    g_ptr_array_add(labels, NULL);

    clawt_lrg_clear(chooser->model_slot);
    chooser->model = clawt_lrg_combo((const gchar *const *)ids->pdata,
                                     (const gchar *const *)labels->pdata,
                                     g_ptr_array_index(ids, 0));
    lrg_widget_set_fill_weight(chooser->model, 1.0f);
    g_signal_connect(chooser->model, "changed",
                     G_CALLBACK(on_model_changed), chooser);
    clawt_lrg_take(chooser->model_slot, chooser->model);
    on_model_changed(NULL, chooser);

    lrg_label_set_text(LRG_LABEL(chooser->note),
                       (note != NULL) ? note : "");
}

/*
 * @require is the flag a provider must carry: "agent" for what
 * libreclaw can drive (anything else is silently rewritten to
 * claude-code), "tools" for what the designer can use.
 */
static void
chooser_fill(ModelChooser *chooser, JsonNode *catalog, const gchar *require)
{
    g_autoptr(GPtrArray) ids = g_ptr_array_new();
    g_autoptr(GPtrArray) labels = g_ptr_array_new();
    guint i;

    g_clear_pointer(&chooser->providers, json_array_unref);
    chooser->providers =
        clawt_lrg_create_filter_providers(catalog, require, NULL);

    for (i = 0; chooser->providers != NULL &&
                i < json_array_get_length(chooser->providers); i++) {
        JsonObject *provider =
            json_array_get_object_element(chooser->providers, i);
        const gchar *id = clawt_ipc_payload_string(provider, "id");
        const gchar *label = clawt_ipc_payload_string(provider, "label");

        if (id == NULL)
            continue;

        g_ptr_array_add(ids, (gpointer)id);
        g_ptr_array_add(labels, (gpointer)((label != NULL) ? label : id));
    }

    g_ptr_array_add(ids, NULL);
    g_ptr_array_add(labels, NULL);

    clawt_lrg_clear(chooser->provider_slot);
    chooser->provider = clawt_lrg_combo(
        (const gchar *const *)ids->pdata, (const gchar *const *)labels->pdata,
        (ids->len > 1) ? g_ptr_array_index(ids, 0) : NULL);
    lrg_widget_set_fill_weight(chooser->provider, 1.0f);
    g_signal_connect(chooser->provider, "changed",
                     G_CALLBACK(on_provider_changed), chooser);
    clawt_lrg_take(chooser->provider_slot, chooser->provider);
    on_provider_changed(NULL, chooser);
}

static void
chooser_build(ModelChooser *chooser, LrgWidget *form,
              const gchar *provider_title, const gchar *model_title)
{
    chooser->provider_slot = clawt_lrg_hbox(0.0f);
    clawt_lrg_take(chooser->provider_slot,
                   clawt_lrg_label("Asking the providers…", "muted"));
    clawt_lrg_form_add(form, provider_title,
                       g_object_ref(chooser->provider_slot), NULL);
    g_object_unref(chooser->provider_slot);

    chooser->note = clawt_lrg_label("", "muted");
    clawt_lrg_add(form, chooser->note);
    g_object_unref(chooser->note);

    chooser->model_slot = clawt_lrg_hbox(0.0f);
    clawt_lrg_form_add(form, model_title, g_object_ref(chooser->model_slot),
                       NULL);
    g_object_unref(chooser->model_slot);

    chooser->model_entry = clawt_lrg_entry(NULL, "a model name");
    clawt_lrg_form_add(form, "Model name", chooser->model_entry, NULL);
    lrg_widget_set_visible(field_of(chooser->model_entry), FALSE);
}

/*
 * A catalogue chooser: container images (by reference) or cloud images
 * (by path), with "Other…" past the end for a value typed by hand.
 */
typedef struct {
    JsonNode   *catalog;      /* owned */
    const gchar *list_member; /* "images" */
    const gchar *id_member;   /* "reference" or "path" */
    const gchar *label_member;
    gboolean    disk;
    LrgWidget  *slot;
    LrgWidget  *combo;
    LrgWidget  *note;
    LrgWidget  *entry;
    LrgWidget  *field;        /* the whole chooser, for hiding */
    gboolean    wanted;       /* the computer type reads it */
} ImageChooser;

static JsonArray *
image_items(ImageChooser *chooser)
{
    return clawt_lrg_settings_member_array(
        clawt_ipc_reply_object(chooser->catalog), chooser->list_member);
}

static JsonObject *
image_chosen(ImageChooser *chooser)
{
    JsonArray *items = image_items(chooser);
    const gchar *id;
    guint i;

    if (chooser->combo == NULL)
        return NULL;

    id = clawt_lrg_combo_id(chooser->combo);

    for (i = 0; items != NULL && i < json_array_get_length(items); i++) {
        JsonObject *item = json_array_get_object_element(items, i);

        if (g_strcmp0(clawt_ipc_payload_string(item, chooser->id_member),
                      id) == 0)
            return item;
    }

    return NULL;
}

/* The value for the payload, or NULL when the chooser does not apply. */
static const gchar *
image_value(ImageChooser *chooser)
{
    JsonObject *item;

    if (!chooser->wanted)
        return NULL;

    item = image_chosen(chooser);

    if (item != NULL)
        return clawt_ipc_payload_string(item, chooser->id_member);

    return answer(chooser->entry);
}

static void
image_update(ImageChooser *chooser)
{
    JsonObject *item = image_chosen(chooser);
    JsonArray *items = image_items(chooser);
    g_autofree gchar *note = NULL;

    lrg_widget_set_visible(chooser->field, chooser->wanted);
    lrg_widget_set_visible(field_of(chooser->entry),
                           chooser->wanted && chooser->combo != NULL &&
                           item == NULL);

    /*
     * The full reference under the label, because "Fedora 44" does not
     * say which registry it comes from; for a disk, where to get one
     * when there is none -- a VM without one defines, starts and never
     * boots.
     */
    note = chooser->disk
        ? g_strdup(clawt_lrg_create_disk_note(
              item, (items != NULL) ? json_array_get_length(items) : 0))
        : clawt_lrg_create_image_note(item);
    lrg_label_set_text(LRG_LABEL(chooser->note), note);
}

static void
on_image_changed(LrgComboBox *combo, gpointer user_data)
{
    image_update(user_data);
}

static void
image_fill(ImageChooser *chooser, JsonNode *reply)
{
    JsonArray *items;
    g_autoptr(GPtrArray) ids = g_ptr_array_new();
    g_autoptr(GPtrArray) labels = g_ptr_array_new();
    guint chosen;
    guint i;

    g_clear_pointer(&chooser->catalog, json_node_unref);
    chooser->catalog = (reply != NULL) ? json_node_copy(reply) : NULL;
    items = image_items(chooser);

    for (i = 0; items != NULL && i < json_array_get_length(items); i++) {
        JsonObject *item = json_array_get_object_element(items, i);
        const gchar *id = clawt_ipc_payload_string(item, chooser->id_member);
        const gchar *label =
            clawt_ipc_payload_string(item, chooser->label_member);

        g_ptr_array_add(ids, (gpointer)((id != NULL) ? id : ""));
        g_ptr_array_add(labels, (gpointer)((label != NULL) ? label : id));
    }

    g_ptr_array_add(ids, (gpointer)OTHER_ID);
    g_ptr_array_add(labels, (gpointer)"Other…");
    g_ptr_array_add(ids, NULL);
    g_ptr_array_add(labels, NULL);

    /* the catalogue's own default for an image; the first disk otherwise */
    chosen = clawt_lrg_create_catalog_index(
        items, chooser->id_member, NULL,
        chooser->disk ? NULL : clawt_ipc_payload_string(
                                   clawt_ipc_reply_object(chooser->catalog),
                                   "default"));

    clawt_lrg_clear(chooser->slot);
    chooser->combo = clawt_lrg_combo((const gchar *const *)ids->pdata,
                                     (const gchar *const *)labels->pdata,
                                     g_ptr_array_index(ids, chosen));
    lrg_widget_set_fill_weight(chooser->combo, 1.0f);
    g_signal_connect(chooser->combo, "changed", G_CALLBACK(on_image_changed),
                     chooser);
    clawt_lrg_take(chooser->slot, chooser->combo);
    image_update(chooser);
}

static void
image_build(ImageChooser *chooser, LrgWidget *form, const gchar *title,
            const gchar *entry_title, const gchar *placeholder)
{
    LrgWidget *box = clawt_lrg_vbox(4.0f);

    chooser->slot = clawt_lrg_hbox(0.0f);
    clawt_lrg_take(chooser->slot, clawt_lrg_label("Asking…", "muted"));
    clawt_lrg_add(box, chooser->slot);
    g_object_unref(chooser->slot);

    chooser->note = clawt_lrg_label("", "muted");
    clawt_lrg_add(box, chooser->note);
    g_object_unref(chooser->note);

    clawt_lrg_form_add(form, title, box, NULL);
    chooser->field = field_of(box);

    chooser->entry = clawt_lrg_entry(NULL, placeholder);
    clawt_lrg_form_add(form, entry_title, chooser->entry, NULL);
    image_update(chooser);
}

/* ── New agent ────────────────────────────────────────────────────── */

typedef struct {
    ClawtLrgApp  *app;
    LrgDialog    *dialog;     /* weak */

    LrgWidget    *id;
    LrgWidget    *name;
    LrgWidget    *description;
    LrgWidget    *team_slot;
    LrgWidget    *team;       /* combo, nullable until team.list */
    LrgWidget    *computer;
    ModelChooser  models;
    ImageChooser  image;
    ImageChooser  disk;

    LrgWidget    *purpose;
    LrgWidget    *boundaries;
    LrgWidget    *needs;
    LrgWidget    *personality;
    LrgWidget    *projects;
    LrgWidget    *notes;
    ModelChooser  designer;
    LrgWidget    *design_button;
    gboolean      designing;
} NewAgent;

static void
new_agent_clear(gpointer data)
{
    NewAgent *dialog = data;

    g_clear_weak_pointer(&dialog->dialog);
    g_clear_pointer(&dialog->models.providers, json_array_unref);
    g_clear_pointer(&dialog->designer.providers, json_array_unref);
    g_clear_pointer(&dialog->image.catalog, json_node_unref);
    g_clear_pointer(&dialog->disk.catalog, json_node_unref);
}

/*
 * Counted: the dialog holds one and each request in flight one more, so
 * a reply arriving after the dialog closed reads freed memory nowhere.
 * Every reply checks dialog->dialog before touching a widget, since the
 * widgets went with it.
 */
static void
new_agent_release(gpointer data)
{
    g_rc_box_release_full(data, new_agent_clear);
}

static const gchar *
dialog_team(NewAgent *dialog)
{
    const gchar *id = (dialog->team != NULL)
        ? clawt_lrg_combo_id(dialog->team) : NULL;

    /* "" is "No team", and means send nothing */
    return (id != NULL && *id != '\0') ? id : NULL;
}

/*
 * The computer the form describes, read by one function for both paths:
 * `agent.create` once sent the disk image and `design.agent` did not, so
 * a designed VM agent refused to provision naming a setting that was
 * filled in on screen.  Each backend gets only its own image key.
 */
static const gchar *
dialog_computer(NewAgent *dialog, const gchar **out_image,
                const gchar **out_disk)
{
    *out_image = image_value(&dialog->image);
    *out_disk = image_value(&dialog->disk);

    return clawt_lrg_combo_id(dialog->computer);
}

/*
 * Asked of the library, never counted: an index here once pointed the
 * image row at the wrong backend the day a type was added before it.
 */
static void
on_computer_changed(LrgComboBox *combo, gpointer user_data)
{
    NewAgent *dialog = user_data;
    gint value = CLAWT_COMPUTER_NONE;

    clawt_enum_from_nick(CLAWT_TYPE_COMPUTER_TYPE,
                         clawt_lrg_combo_id(dialog->computer), &value);

    dialog->image.wanted =
        clawt_computer_type_takes_image((ClawtComputerType)value);
    dialog->disk.wanted = (ClawtComputerType)value == CLAWT_COMPUTER_VM;
    image_update(&dialog->image);
    image_update(&dialog->disk);
    clawt_lrg_app_wake(dialog->app);
}

static void
on_created(ClawtLrgApp *app, JsonNode *reply, const GError *error,
           gpointer user_data)
{
    NewAgent *dialog = user_data;
    JsonObject *root = clawt_ipc_reply_object(reply);
    const gchar *agent_id;
    g_autofree gchar *text = NULL;

    /* the dialog stays open on failure, with what was typed intact */
    if (reply == NULL)
        return;

    agent_id = clawt_ipc_payload_string(root, "id");

    if (agent_id == NULL && dialog->dialog != NULL)
        agent_id = answer(dialog->id);

    /* what actually happened, not "Agent created." whatever followed */
    text = clawt_lrg_create_created_text(root, agent_id);
    clawt_lrg_toast(app, text);

    select_new_agent(app, agent_id);

    if (dialog->dialog != NULL)
        lrg_dialog_close(dialog->dialog);
}

static void
on_create_clicked(LrgButton *button, gpointer user_data)
{
    NewAgent *dialog = user_data;
    const gchar *agent_id = answer(dialog->id);
    const gchar *image;
    const gchar *disk;
    const gchar *computer;

    if (agent_id == NULL) {
        clawt_lrg_toast(dialog->app, "An agent needs an id.");
        return;
    }

    computer = dialog_computer(dialog, &image, &disk);

    clawt_lrg_request(dialog->app, "agent.create",
                      clawt_ipc_payload_new(
                          "id", agent_id,
                          "name", answer(dialog->name),
                          "description", answer(dialog->description),
                          "provider", chooser_provider_id(&dialog->models),
                          "model", chooser_model(&dialog->models),
                          "computer", computer,
                          "image", image,
                          "vm_image", disk,
                          "team", dialog_team(dialog),
                          NULL),
                      CLAWT_LRG_REQUEST_NONE, on_created,
                      g_rc_box_acquire(dialog), new_agent_release);
}

/* ── Proposed agent ───────────────────────────────────────────────── */

typedef struct {
    ClawtLrgApp *app;
    gchar       *draft;
    gboolean     committed;
    NewAgent    *outer;       /* a reference */
} Proposal;

static void
on_discarded(ClawtLrgApp *app, JsonNode *reply, const GError *error,
             gpointer user_data)
{
}

/*
 * The preview went away.  Unless it was committed, the draft is dropped
 * rather than left on the daemon: it holds a whole designer, and a
 * person who said no is done with it.  Here, at finalisation, because
 * Cancel, Escape and the veil all close the dialog without a response.
 */
static void
proposal_free(gpointer data)
{
    Proposal *proposal = data;

    if (!proposal->committed && proposal->draft != NULL)
        clawt_lrg_request(proposal->app, "design.discard",
                          clawt_ipc_payload_new("draft", proposal->draft,
                                                NULL),
                          CLAWT_LRG_QUIET, on_discarded, NULL, NULL);

    new_agent_release(proposal->outer);
    g_free(proposal->draft);
    g_free(proposal);
}

static void
on_committed(ClawtLrgApp *app, JsonNode *reply, const GError *error,
             gpointer user_data)
{
    NewAgent *outer = user_data;
    JsonObject *root = clawt_ipc_reply_object(reply);
    const gchar *agent_id = clawt_ipc_payload_string(root, "id");
    g_autofree gchar *text = NULL;

    if (reply == NULL)
        return;

    text = clawt_lrg_create_created_text(root, agent_id);
    clawt_lrg_toast(app, text);
    select_new_agent(app, agent_id);

    /*
     * The outer dialog closes too.  GTK's once did not, leaving New
     * agent open over a fleet that already contained it.
     */
    if (outer->dialog != NULL)
        lrg_dialog_close(outer->dialog);
}

/*
 * Commits the draft that was reviewed.  Re-running the design would be
 * a fresh conversation producing something else, which would make the
 * preview a demonstration rather than a decision.
 */
static gboolean
on_proposal_response(ClawtLrgApp *app, LrgDialog *dialog, gint response,
                     gpointer user_data)
{
    Proposal *proposal = user_data;

    proposal->committed = TRUE;
    clawt_lrg_request(app, "design.commit",
                      clawt_ipc_payload_new("draft", proposal->draft, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_committed,
                      g_rc_box_acquire(proposal->outer), new_agent_release);

    return TRUE;
}

static void
on_designed(ClawtLrgApp *app, JsonNode *reply, const GError *error,
            gpointer user_data)
{
    NewAgent *dialog = user_data;
    JsonObject *result = clawt_ipc_reply_object(reply);
    g_autofree gchar *body = NULL;
    LrgWidget *view;
    Proposal *proposal;
    const gchar *draft;

    dialog->designing = FALSE;

    if (dialog->dialog != NULL) {
        lrg_button_set_text(LRG_BUTTON(dialog->design_button), "Design it");
        lrg_widget_set_enabled(dialog->design_button, TRUE);
    }

    draft = clawt_ipc_payload_string(result, "draft");

    if (result == NULL || draft == NULL)
        return;

    /*
     * Shown before anything is written: a model's proposal becoming an
     * agent without a person reading it first is the convenience nobody
     * asks for twice.
     */
    body = clawt_lrg_create_preview_text(result);
    view = clawt_lrg_editor(body, "yaml");
    lrg_text_editor_set_read_only(LRG_TEXT_EDITOR(view), TRUE);
    lrg_widget_add_class(view, "clawt-terminal");
    lrg_widget_set_min_size(view, 620.0f * (gfloat)app->scale,
                            380.0f * (gfloat)app->scale);

    proposal = g_new0(Proposal, 1);
    proposal->app = app;
    proposal->draft = g_strdup(draft);
    proposal->outer = g_rc_box_acquire(dialog);

    g_object_unref(clawt_lrg_dialog(
        app, "Proposed agent",
        "Nothing is written until you create it. The org files are listed "
        "rather than shown; they are on disk after Create.",
        view, "Create", FALSE, on_proposal_response, proposal,
        proposal_free));
    g_object_unref(view);
}

static void
on_design_clicked(LrgButton *button, gpointer user_data)
{
    NewAgent *dialog = user_data;
    const gchar *purpose = answer(dialog->purpose);
    const gchar *image;
    const gchar *disk;
    const gchar *computer;

    if (purpose == NULL) {
        clawt_lrg_toast(dialog->app, "Say what the agent should do first.");
        return;
    }

    /*
     * Disabled while the model works: a design takes tens of seconds,
     * and a second click starts a second one, billed and discarded.
     */
    if (dialog->designing)
        return;

    dialog->designing = TRUE;
    lrg_button_set_text(LRG_BUTTON(dialog->design_button), "Designing…");
    lrg_widget_set_enabled(dialog->design_button, FALSE);

    computer = dialog_computer(dialog, &image, &disk);

    clawt_lrg_request(
        dialog->app, "design.agent",
        clawt_ipc_payload_new(
            /*
             * Whatever was typed above is pinned, so the model fills in
             * what is blank rather than renaming an agent somebody
             * already named.
             */
            "id", answer(dialog->id),
            "name", answer(dialog->name),
            "purpose", purpose,
            "boundaries", answer(dialog->boundaries),
            "needs", answer(dialog->needs),
            "personality", answer(dialog->personality),
            "projects", answer(dialog->projects),
            "notes", answer(dialog->notes),
            "provider", chooser_provider_id(&dialog->designer),
            "model", chooser_model(&dialog->designer),
            /*
             * The computer and its image, and the team: choices made on
             * this form that the model has no way to know.  The designer
             * cannot name a disk image -- the ones that exist are the
             * ones somebody fetched.
             */
            "computer", (g_strcmp0(computer, "none") != 0) ? computer
                                                           : NULL,
            "image", image,
            "vm_image", disk,
            "team", dialog_team(dialog),
            NULL),
        CLAWT_LRG_REQUEST_NONE, on_designed, g_rc_box_acquire(dialog),
        new_agent_release);
}

/* ── New agent: filling the slots ─────────────────────────────────── */

static void
on_teams(ClawtLrgApp *app, JsonNode *reply, const GError *error,
         gpointer user_data)
{
    NewAgent *dialog = user_data;
    g_autoptr(GPtrArray) ids = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) labels = g_ptr_array_new_with_free_func(g_free);

    if (dialog->dialog == NULL)
        return;

    clawt_lrg_create_team_choices(reply, NULL, ids, labels);
    g_ptr_array_add(ids, NULL);
    g_ptr_array_add(labels, NULL);

    clawt_lrg_clear(dialog->team_slot);
    dialog->team = clawt_lrg_combo((const gchar *const *)ids->pdata,
                                   (const gchar *const *)labels->pdata, "");
    lrg_widget_set_fill_weight(dialog->team, 1.0f);
    clawt_lrg_take(dialog->team_slot, dialog->team);
}

static void
on_models(ClawtLrgApp *app, JsonNode *reply, const GError *error,
          gpointer user_data)
{
    NewAgent *dialog = user_data;

    if (dialog->dialog == NULL)
        return;

    chooser_fill(&dialog->models, reply, "agent");
    chooser_fill(&dialog->designer, reply, "tools");
}

static void
on_images(ClawtLrgApp *app, JsonNode *reply, const GError *error,
          gpointer user_data)
{
    NewAgent *dialog = user_data;

    if (dialog->dialog != NULL)
        image_fill(&dialog->image, reply);
}

static void
on_disks(ClawtLrgApp *app, JsonNode *reply, const GError *error,
         gpointer user_data)
{
    NewAgent *dialog = user_data;

    if (dialog->dialog != NULL)
        image_fill(&dialog->disk, reply);
}

/* ── New agent: building ──────────────────────────────────────────── */

static LrgWidget *
build_by_hand(NewAgent *dialog)
{
    LrgWidget *body;
    LrgWidget *card = group_card("By hand", NULL, &body);
    LrgWidget *form = clawt_lrg_form_new();
    g_autoptr(GPtrArray) ids = g_ptr_array_new();
    g_autoptr(GPtrArray) labels = g_ptr_array_new();
    LrgWidget *create;
    guint i;

    dialog->id = clawt_lrg_entry(NULL, "researcher");
    clawt_lrg_form_add(form, "Id", dialog->id, NULL);

    /*
     * Name and description here, not only in the inspector: the
     * description is what other agents read when deciding who to
     * delegate to.
     */
    dialog->name = clawt_lrg_entry(NULL, "Researcher");
    clawt_lrg_form_add(form, "Name", dialog->name, NULL);
    dialog->description = clawt_lrg_entry(NULL, "What it is for");
    clawt_lrg_form_add(form, "What it is for", dialog->description, NULL);

    /*
     * The team here rather than only afterwards: an agent made for a
     * team and left out of it is one the lead cannot hand anything to.
     */
    dialog->team_slot = clawt_lrg_hbox(0.0f);
    clawt_lrg_take(dialog->team_slot, clawt_lrg_label("Asking…", "muted"));
    clawt_lrg_form_add(form, "Team", g_object_ref(dialog->team_slot), NULL);
    g_object_unref(dialog->team_slot);

    chooser_build(&dialog->models, form, "Provider", "Model");

    /* walked from the library, never a list of its own */
    for (i = 0; i < clawt_computer_type_count(); i++) {
        g_ptr_array_add(ids, (gpointer)clawt_computer_type_nth_nick(i));
        g_ptr_array_add(labels, (gpointer)clawt_computer_type_nth_label(i));
    }

    g_ptr_array_add(ids, NULL);
    g_ptr_array_add(labels, NULL);
    dialog->computer = clawt_lrg_combo((const gchar *const *)ids->pdata,
                                       (const gchar *const *)labels->pdata,
                                       clawt_computer_type_nth_nick(0));
    g_signal_connect(dialog->computer, "changed",
                     G_CALLBACK(on_computer_changed), dialog);
    clawt_lrg_form_add(form, "Computer", dialog->computer, NULL);

    /*
     * The image rows exist only while the chosen computer reads them: a
     * greyed Image row on a host agent still reads as "this agent has an
     * image", which is not true.
     */
    dialog->image.list_member = "images";
    dialog->image.id_member = "reference";
    dialog->image.label_member = "label";
    image_build(&dialog->image, form, "Image", "Image reference",
                "any reference podman can pull");

    dialog->disk.list_member = "images";
    dialog->disk.id_member = "path";
    dialog->disk.label_member = "name";
    dialog->disk.disk = TRUE;
    image_build(&dialog->disk, form, "Disk image", "Path to a qcow2",
                "/var/lib/…/image.qcow2");

    on_computer_changed(NULL, dialog);

    create = clawt_lrg_button("Create", "primary",
                              G_CALLBACK(on_create_clicked), dialog);
    lrg_widget_set_halign(create, LRG_ALIGN_START);
    clawt_lrg_take(form, create);

    clawt_lrg_take(body, form);
    return card;
}

static LrgWidget *
build_by_description(NewAgent *dialog)
{
    LrgWidget *body;
    LrgWidget *card = group_card(
        "By description",
        "Answer what you can and clawtilla drafts the configuration and the "
        "agent's org files. Only the first is required; a blank answer "
        "becomes a heading to fill in rather than an invention. You see "
        "everything before anything is created.", &body);
    LrgWidget *form = clawt_lrg_form_new();
    struct {
        const gchar *question;
        LrgWidget  **entry;
    } questions[6];
    guint i;

    /*
     * Named questions rather than one box: a paragraph that leaves out
     * the boundaries produces an agent with none.
     */
    questions[0].question = "What should it do?";
    questions[0].entry = &dialog->purpose;
    questions[1].question = "What should it never do?";
    questions[1].entry = &dialog->boundaries;
    questions[2].question = "What does it need to work on?";
    questions[2].entry = &dialog->needs;
    questions[3].question = "How should it come across?";
    questions[3].entry = &dialog->personality;
    questions[4].question = "What is it working on, and where?";
    questions[4].entry = &dialog->projects;
    questions[5].question = "Anything else it should know?";
    questions[5].entry = &dialog->notes;

    for (i = 0; i < G_N_ELEMENTS(questions); i++) {
        *questions[i].entry = clawt_lrg_entry(NULL, NULL);
        clawt_lrg_form_add(form, questions[i].question,
                           *questions[i].entry, NULL);
    }

    /*
     * Which model designs it, separate from the one the agent will run:
     * somebody will often want their best model to draft an agent that
     * then runs on a cheap one.
     */
    chooser_build(&dialog->designer, form, "Designed by",
                  "Designer's model");

    dialog->design_button = clawt_lrg_button(
        "Design it", NULL, G_CALLBACK(on_design_clicked), dialog);
    lrg_widget_set_halign(dialog->design_button, LRG_ALIGN_START);
    clawt_lrg_add(form, dialog->design_button);
    g_object_unref(dialog->design_button);

    clawt_lrg_take(body, form);
    return card;
}

void
clawt_lrg_new_agent_dialog(ClawtLrgApp *app)
{
    NewAgent *dialog = g_rc_box_new0(NewAgent);
    LrgWidget *content = clawt_lrg_vbox(16.0f);
    LrgWidget *scroll;
    LrgDialog *shown;

    dialog->app = app;

    clawt_lrg_take(content, build_by_hand(dialog));
    clawt_lrg_take(content, build_by_description(dialog));
    scroll = scroller(app, content, 640.0f, 600.0f);
    g_object_unref(content);

    /* no primary: Create and Design it sit with the answers they read */
    shown = clawt_lrg_dialog(app, "New agent", NULL, scroll, NULL, FALSE,
                             NULL, dialog, new_agent_release);
    g_set_weak_pointer(&dialog->dialog, shown);
    g_object_unref(shown);
    g_object_unref(scroll);

    clawt_lrg_request(app, "team.list", NULL, CLAWT_LRG_QUIET, on_teams,
                      g_rc_box_acquire(dialog), new_agent_release);

    /*
     * refresh: the provider's own list where it can be had.  The daemon
     * falls back to its table without a key or a network, so this never
     * produces an empty chooser -- it only takes longer.
     */
    clawt_lrg_request(app, "model.list",
                      clawt_ipc_payload_new("refresh", "true", NULL),
                      CLAWT_LRG_REQUEST_NONE, on_models,
                      g_rc_box_acquire(dialog), new_agent_release);
    clawt_lrg_request(app, "image.list", NULL, CLAWT_LRG_QUIET, on_images,
                      g_rc_box_acquire(dialog), new_agent_release);
    clawt_lrg_request(app, "image.vm_list", NULL, CLAWT_LRG_QUIET, on_disks,
                      g_rc_box_acquire(dialog), new_agent_release);
}

/* ── Import an agent ──────────────────────────────────────────────── */

typedef struct {
    ClawtLrgApp *app;
    LrgDialog   *dialog;    /* weak */
    LrgWidget   *found;
    LrgWidget   *id;
    LrgWidget   *path;
    LrgWidget   *mode;
    LrgWidget   *url;
    LrgWidget   *keep_git;
} Import;

static void
import_clear(gpointer data)
{
    Import *import = data;

    g_clear_weak_pointer(&import->dialog);
}

static void
import_release(gpointer data)
{
    g_rc_box_release_full(data, import_clear);
}

typedef struct {
    Import *import;     /* a reference */
    gchar  *agent_id;
} Adopt;

static void
adopt_free(gpointer data)
{
    Adopt *adopt = data;

    import_release(adopt->import);
    g_free(adopt->agent_id);
    g_free(adopt);
}

static void
on_adopted(ClawtLrgApp *app, JsonNode *reply, const GError *error,
           gpointer user_data)
{
    Adopt *adopt = user_data;
    const gchar *detail;

    if (reply == NULL)
        return;

    /*
     * The daemon's own sentence: whether a git import became a submodule
     * depends on the machine, so the side that found out says.
     */
    detail = clawt_ipc_payload_string(clawt_ipc_reply_object(reply),
                                      "detail");
    clawt_lrg_toast(app, (detail != NULL)
                             ? detail
                             : "Imported. Check it over before starting it.");
    select_new_agent(app, adopt->agent_id);

    if (adopt->import->dialog != NULL)
        lrg_dialog_close(adopt->import->dialog);
}

/*
 * Adopting is agent.create with nothing but an id: the workspace and the
 * mailbox are already where the daemon looks, so there is deliberately
 * no second path that could treat an adopted agent differently.
 */
static void
on_adopt_clicked(LrgButton *button, gpointer user_data)
{
    Import *import = user_data;
    const gchar *agent_id =
        g_object_get_data(G_OBJECT(button), "clawt-agent");
    Adopt *adopt = g_new0(Adopt, 1);

    adopt->import = g_rc_box_acquire(import);
    adopt->agent_id = g_strdup(agent_id);

    clawt_lrg_request(import->app, "agent.create",
                      clawt_ipc_payload_new("id", agent_id, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_adopted, adopt, adopt_free);
}

/*
 * What is on disk and in no config.  Each row says what it holds:
 * a workspace with a mailbox and memories is an agent somebody ran;
 * one with only config.yaml is a design abandoned halfway.
 */
static void
on_discovered(ClawtLrgApp *app, JsonNode *reply, const GError *error,
              gpointer user_data)
{
    Import *import = user_data;
    JsonArray *found = clawt_lrg_settings_member_array(
        clawt_ipc_reply_object(reply), "found");
    guint shown = 0;
    guint i;

    if (import->dialog == NULL)
        return;

    clawt_lrg_clear(import->found);

    for (i = 0; found != NULL && i < json_array_get_length(found); i++) {
        JsonObject *entry = json_array_get_object_element(found, i);
        const gchar *agent_id = clawt_ipc_payload_string(entry, "id");
        LrgWidget *row;
        LrgWidget *adopt;

        if (agent_id == NULL)
            continue;

        row = clawt_lrg_row(agent_id,
                            clawt_lrg_create_found_subtitle(entry));
        adopt = clawt_lrg_button("Import", "primary",
                                 G_CALLBACK(on_adopt_clicked), import);
        g_object_set_data_full(G_OBJECT(adopt), "clawt-agent",
                               g_strdup(agent_id), g_free);
        clawt_lrg_row_add_suffix(row, adopt);
        clawt_lrg_appear(app, row, shown++);
        clawt_lrg_take(import->found, row);
    }

    if (shown == 0)
        clawt_lrg_take(import->found, clawt_lrg_row(
            (reply != NULL) ? "Nothing unclaimed on disk"
                            : "Could not ask the daemon",
            (reply != NULL) ? "Every workspace clawtilla can see is already "
                              "an agent." : NULL));

    clawt_lrg_app_wake(app);
}

/*
 * A git import names a URL; the other two a directory.  Asked of the
 * library (clawt_import_mode_takes_url()) rather than compared against
 * "git" here.  Insensitive rather than hidden: a row that vanishes
 * takes the reader's place with it.  Only a copy reads the source's
 * .git, so the switch means nothing for the other two.
 */
static void
on_mode_changed(LrgComboBox *combo, gpointer user_data)
{
    Import *import = user_data;
    ClawtImportMode mode =
        clawt_import_mode_from_nick(clawt_lrg_combo_id(import->mode));
    gboolean wants_url = clawt_import_mode_takes_url(mode);

    lrg_widget_set_enabled(import->url, wants_url);
    lrg_widget_set_enabled(import->path, !wants_url);
    lrg_widget_set_enabled(import->keep_git, mode == CLAWT_IMPORT_COPY);
}

static void
on_folder_chosen(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Import *import = user_data;
    g_autoptr(GFile) folder = NULL;
    g_autoptr(GError) error = NULL;

    folder = lrg_file_dialog_select_folder_finish(LRG_FILE_DIALOG(source),
                                                  result, &error);

    /* dismissing the chooser is not a failure worth a toast */
    if (folder != NULL && import->dialog != NULL) {
        g_autofree gchar *path = g_file_get_path(folder);

        lrg_text_input_set_text(LRG_TEXT_INPUT(import->path),
                                (path != NULL) ? path : "");

        /*
         * The id defaults to the directory's own name, right often enough
         * to fill in and still editable when it is not.
         */
        if (answer(import->id) == NULL) {
            g_autofree gchar *base = g_file_get_basename(folder);

            lrg_text_input_set_text(LRG_TEXT_INPUT(import->id), base);
        }
    }

    import_release(import);
}

static void
on_choose_folder(LrgButton *button, gpointer user_data)
{
    Import *import = user_data;
    g_autoptr(LrgFileDialog) chooser = lrg_file_dialog_new();

    lrg_file_dialog_set_title(chooser, "The agent's workspace");
    lrg_file_dialog_select_folder(chooser, LRG_WIDGET(import->app->canvas),
                                  NULL, on_folder_chosen,
                                  g_rc_box_acquire(import));
}

static void
on_imported(ClawtLrgApp *app, JsonNode *reply, const GError *error,
            gpointer user_data)
{
    Adopt *adopt = user_data;

    /* left open on failure, so the path and id are there to fix */
    if (reply == NULL)
        return;

    clawt_lrg_toast(app, "Imported. Check it over before starting it.");
    select_new_agent(app, adopt->agent_id);

    if (adopt->import->dialog != NULL)
        lrg_dialog_close(adopt->import->dialog);
}

static void
on_import_clicked(LrgButton *button, gpointer user_data)
{
    Import *import = user_data;
    g_autoptr(GError) error = NULL;
    Adopt *adopt;
    JsonNode *payload;

    payload = clawt_lrg_create_import_payload(
        answer(import->id),
        clawt_import_mode_from_nick(clawt_lrg_combo_id(import->mode)),
        answer(import->url), answer(import->path),
        clawt_lrg_switch_active(import->keep_git), &error);

    if (payload == NULL) {
        clawt_lrg_toast(import->app, error->message);
        return;
    }

    adopt = g_new0(Adopt, 1);
    adopt->import = g_rc_box_acquire(import);
    adopt->agent_id = g_strdup(answer(import->id));

    clawt_lrg_request(import->app, "agent.import", payload,
                      CLAWT_LRG_REQUEST_NONE, on_imported, adopt,
                      adopt_free);
}

void
clawt_lrg_import_agent_dialog(ClawtLrgApp *app)
{
    Import *import = g_rc_box_new0(Import);
    LrgWidget *content = clawt_lrg_vbox(16.0f);
    LrgWidget *found_body;
    LrgWidget *from_body;
    LrgWidget *form = clawt_lrg_form_new();
    LrgWidget *path_row = clawt_lrg_hbox(8.0f);
    LrgWidget *import_button;
    LrgWidget *scroll;
    LrgDialog *shown;
    g_autoptr(GPtrArray) ids = g_ptr_array_new();
    g_autoptr(GPtrArray) labels = g_ptr_array_new();
    guint i;

    import->app = app;

    clawt_lrg_take(content, group_card(
        "Already on disk",
        "Workspaces clawtilla can see that no agent claims. Importing one "
        "adds the config entry it is missing; nothing is copied or moved.",
        &found_body));
    import->found = found_body;
    clawt_lrg_take(import->found,
                   clawt_lrg_label("Looking…", "muted"));

    clawt_lrg_take(content, group_card(
        "From somewhere else",
        "Copies a standalone libreclaw workspace in. Its provider and model "
        "come with it, so an import does not quietly move the agent onto "
        "the fleet defaults.", &from_body));

    import->id = clawt_lrg_entry(NULL, "the directory's name");
    clawt_lrg_form_add(form, "Id", import->id, NULL);

    import->path = clawt_lrg_entry(NULL, "Nothing chosen");
    lrg_widget_set_fill_weight(import->path, 1.0f);
    clawt_lrg_add(path_row, import->path);
    g_object_unref(import->path);
    clawt_lrg_take(path_row, clawt_lrg_button("Choose…", NULL,
                                              G_CALLBACK(on_choose_folder),
                                              import));
    clawt_lrg_form_add(form, "Workspace", path_row, NULL);

    /* walked from the library, as both other clients do */
    for (i = 0; i < clawt_import_mode_count(); i++) {
        g_ptr_array_add(ids, (gpointer)clawt_import_mode_nth_nick(i));
        g_ptr_array_add(labels, (gpointer)clawt_import_mode_nth_label(i));
    }

    g_ptr_array_add(ids, NULL);
    g_ptr_array_add(labels, NULL);
    import->mode = clawt_lrg_combo((const gchar *const *)ids->pdata,
                                   (const gchar *const *)labels->pdata,
                                   clawt_import_mode_nth_nick(0));
    g_signal_connect(import->mode, "changed", G_CALLBACK(on_mode_changed),
                     import);
    clawt_lrg_form_add(form, "How", import->mode, NULL);

    import->url = clawt_lrg_entry(NULL, "https://…/agent.git");
    clawt_lrg_form_add(form, "Git URL", import->url, NULL);

    import->keep_git = clawt_lrg_switch("Keep git history", FALSE);
    clawt_lrg_form_add(form, NULL, import->keep_git,
                       "Copies .git too. Off by default: the new workspace "
                       "is usually not the same repository.");

    import_button = clawt_lrg_button("Import", "primary",
                                     G_CALLBACK(on_import_clicked), import);
    lrg_widget_set_halign(import_button, LRG_ALIGN_START);
    clawt_lrg_take(form, import_button);
    clawt_lrg_take(from_body, form);

    on_mode_changed(NULL, import);

    scroll = scroller(app, content, 600.0f, 520.0f);
    g_object_unref(content);

    shown = clawt_lrg_dialog(app, "Import an agent", NULL, scroll, NULL,
                             FALSE, NULL, import, import_release);
    g_set_weak_pointer(&import->dialog, shown);
    g_object_unref(shown);
    g_object_unref(scroll);

    clawt_lrg_request(app, "agent.discover", NULL, CLAWT_LRG_REQUEST_NONE,
                      on_discovered, g_rc_box_acquire(import),
                      import_release);
}

/* ── New room ─────────────────────────────────────────────────────── */

typedef struct {
    LrgWidget *id;
    LrgWidget *name;
    GPtrArray *members;     /* switches, unowned */
} NewRoom;

static void
new_room_free(gpointer data)
{
    NewRoom *room = data;

    g_ptr_array_unref(room->members);
    g_free(room);
}

typedef struct {
    gchar     *room_id;
    guint      n_members;
    LrgDialog *dialog;      /* weak */
} RoomCreated;

static void
room_created_free(gpointer data)
{
    RoomCreated *created = data;

    g_clear_weak_pointer(&created->dialog);
    g_free(created->room_id);
    g_free(created);
}

static void
on_rooms_for_select(ClawtLrgApp *app, JsonNode *reply, const GError *error,
                    gpointer user_data)
{
    if (reply != NULL) {
        g_clear_pointer(&app->rooms, json_node_unref);
        app->rooms = json_node_copy(reply);
    }

    clawt_lrg_select_room(app, user_data);
}

static void
on_room_created(ClawtLrgApp *app, JsonNode *reply, const GError *error,
                gpointer user_data)
{
    RoomCreated *created = user_data;
    const gchar *room_id;

    if (reply == NULL)
        return;

    /*
     * More than two is a group, and a group delivers by mention: said
     * now, because this is the moment somebody is about to type in it.
     */
    clawt_lrg_toast(app, clawt_lrg_create_room_created_text(
                             created->n_members));

    if (created->dialog != NULL)
        lrg_dialog_close(created->dialog);

    room_id = clawt_ipc_payload_string(clawt_ipc_reply_object(reply),
                                       "room");

    clawt_lrg_request(app, "room.list", NULL, CLAWT_LRG_QUIET,
                      on_rooms_for_select,
                      g_strdup((room_id != NULL) ? room_id
                                                 : created->room_id),
                      g_free);
}

static gboolean
on_room_response(ClawtLrgApp *app, LrgDialog *dialog, gint response,
                 gpointer user_data)
{
    NewRoom *room = user_data;
    g_autoptr(GPtrArray) chosen = g_ptr_array_new();
    g_autoptr(GError) error = NULL;
    RoomCreated *created;
    JsonNode *payload;
    guint i;

    for (i = 0; i < room->members->len; i++) {
        LrgWidget *one = g_ptr_array_index(room->members, i);

        if (clawt_lrg_switch_active(one))
            g_ptr_array_add(chosen,
                            g_object_get_data(G_OBJECT(one), "clawt-agent"));
    }

    g_ptr_array_add(chosen, NULL);

    payload = clawt_lrg_create_room_payload(
        answer(room->id), answer(room->name),
        (const gchar *const *)chosen->pdata, &error);

    if (payload == NULL) {
        clawt_lrg_toast(app, error->message);
        return FALSE;
    }

    created = g_new0(RoomCreated, 1);
    created->room_id = g_strdup(answer(room->id));
    created->n_members = chosen->len - 1;
    g_set_weak_pointer(&created->dialog, dialog);

    clawt_lrg_request(app, "room.create", payload, CLAWT_LRG_REQUEST_NONE,
                      on_room_created, created, room_created_free);

    /* closed by the reply, so a refused id is still there to fix */
    return FALSE;
}

void
clawt_lrg_new_room_dialog(ClawtLrgApp *app)
{
    NewRoom *room = g_new0(NewRoom, 1);
    LrgWidget *content = clawt_lrg_vbox(12.0f);
    LrgWidget *form = clawt_lrg_form_new();
    LrgWidget *members = clawt_lrg_vbox(6.0f);
    JsonArray *agents = clawt_lrg_settings_member_array(
        clawt_ipc_reply_object(app->agents), "agents");
    LrgWidget *scroll;
    guint i;

    room->members = g_ptr_array_new();
    room->id = clawt_lrg_entry(NULL, "launch-team");
    room->name = clawt_lrg_entry(NULL, "the id, when empty");
    clawt_lrg_form_add(form, "Id", room->id, NULL);
    clawt_lrg_form_add(form, "Name (optional)", room->name, NULL);
    clawt_lrg_take(content, form);

    clawt_lrg_take(content, clawt_lrg_label("Members", "heading"));
    clawt_lrg_take(content, clawt_lrg_paragraph(
        "Everyone here can read everything said in the room. With more than "
        "two, a message is only delivered to the members it names with "
        "@their-id -- so being in a room does not mean answering everything "
        "in it.", "muted"));

    /* the fleet as the window last read it */
    for (i = 0; agents != NULL && i < json_array_get_length(agents); i++) {
        JsonObject *agent = json_array_get_object_element(agents, i);
        const gchar *id = clawt_ipc_payload_string(agent, "id");
        const gchar *name = clawt_ipc_payload_string(agent, "name");
        g_autofree gchar *text = NULL;
        LrgWidget *one;

        if (id == NULL)
            continue;

        text = (name != NULL && g_strcmp0(name, id) != 0)
            ? g_strdup_printf("%s  (%s)", name, id) : g_strdup(id);
        one = clawt_lrg_switch(text, FALSE);
        g_object_set_data_full(G_OBJECT(one), "clawt-agent", g_strdup(id),
                               g_free);
        g_ptr_array_add(room->members, one);
        clawt_lrg_appear(app, one, i);
        clawt_lrg_take(members, one);
    }

    if (room->members->len == 0)
        clawt_lrg_take(members, clawt_lrg_paragraph(
            "No agents yet. A room needs at least two.", "muted"));

    clawt_lrg_take(content, members);

    scroll = scroller(app, content, 480.0f, 420.0f);
    g_object_unref(content);

    g_object_unref(clawt_lrg_dialog(app, "New room", NULL, scroll, "Create",
                                    FALSE, on_room_response, room,
                                    new_room_free));
    g_object_unref(scroll);
}
