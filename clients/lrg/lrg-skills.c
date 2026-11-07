/*
 * lrg-skills.c - The Skills page: the library, and teaching a task
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Mirrors the Skills page half of clients/gtk/gtk-skills.c (the `/name`
 * half belongs to the composer, in lrg-chat.c).
 *
 * The recordings come first, because a running recording is something
 * happening to somebody's screen right now and the library will still be
 * there later.  The library is a grid of tiles -- name, state, warning
 * count -- with the selected skill opened underneath in full: every
 * warning above its buttons, then what was not copied, where it came
 * from and where it lives.  GTK draws every skill open at once; a grid
 * with one open reads better once there are more than a handful, and
 * the open one carries everything GTK's group does.
 */

#include "clawt-lrg.h"
#include "clawt-lrg-fleet-model.h"

#define SKILL_INDEX_KEY "clawt-skill-index"

static struct {
    ClawtLrgApp *app;
    LrgWidget   *page;
    LrgWidget   *teach_box;     /* the recordings */
    LrgWidget   *problems;      /* skill.list's problems, as banners */
    LrgWidget   *library;       /* tiles, or why there are none */
    LrgWidget   *detail;        /* the selected skill, open */
    LrgTileView *tiles;
    GListStore  *items;
    JsonNode    *skills;        /* the last skill.list reply */
    gchar       *selected;      /* the open skill's name */
    gboolean     filling;       /* the tiles are being refilled */
} state;

static void refresh_skills_library(ClawtLrgApp *app);

/* ── Small pieces ─────────────────────────────────────────────────── */

static JsonArray *
skills_array(void)
{
    JsonObject *root = clawt_ipc_reply_object(state.skills);

    if (root == NULL || !json_object_has_member(root, "skills"))
        return NULL;

    return json_object_get_array_member(root, "skills");
}

static JsonObject *
skill_named(const gchar *name)
{
    JsonArray *skills = skills_array();
    guint i;

    for (i = 0; skills != NULL && i < json_array_get_length(skills); i++) {
        JsonObject *skill = json_array_get_object_element(skills, i);

        if (g_strcmp0(clawt_ipc_payload_string(skill, "name"), name) == 0)
            return skill;
    }

    return NULL;
}

/* A labelled line: a toned badge naming it, then what it says. */
static LrgWidget *
note_line(ClawtLrgNote *note)
{
    LrgWidget *line = clawt_lrg_hbox(10.0f);
    LrgWidget *text = clawt_lrg_paragraph(note->body, NULL);

    lrg_widget_set_fill_weight(text, 1.0f);
    clawt_lrg_take(line, clawt_lrg_badge(note->title, note->tone));
    clawt_lrg_take(line, text);

    return line;
}

/* Any reply that changed the library: re-read it. */
static void
on_changed(ClawtLrgApp *app, JsonNode *reply, const GError *error,
           gpointer user_data)
{
    const gchar *toast = user_data;

    if (reply == NULL)
        return;

    if (toast != NULL)
        clawt_lrg_toast(app, toast);

    refresh_skills_library(app);
}

/* ── Form dialogs ─────────────────────────────────────────────────── */

/*
 * The fields of an open form.  Borrowed: the dialog's content holds
 * them, and this lives exactly as long as the dialog does.
 */
typedef struct {
    LrgWidget *first;
    LrgWidget *second;
    LrgWidget *third;
} Fields;

static gboolean
on_import_response(ClawtLrgApp *app, LrgDialog *dialog, gint response,
                   gpointer user_data)
{
    Fields *fields = user_data;
    g_autofree gchar *refusal = NULL;
    JsonNode *payload = clawt_lrg_skill_import_payload(
        clawt_lrg_entry_text(fields->first),
        clawt_lrg_entry_text(fields->second), &refusal);

    /* refused here: the dialog stays up with what was typed */
    if (payload == NULL) {
        clawt_lrg_toast(app, refusal);
        return FALSE;
    }

    clawt_lrg_request(app, "skill.import", payload, CLAWT_LRG_REQUEST_NONE,
                      on_changed,
                      (gpointer)"Imported, disabled. Read it, then enable it.",
                      NULL);
    return TRUE;
}

static void
on_import_clicked(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    LrgWidget *form = clawt_lrg_form_new();
    Fields *fields = g_new0(Fields, 1);

    fields->first = clawt_lrg_entry(NULL, "/path/to/a/skill");
    fields->second = clawt_lrg_entry(NULL, "https://… (optional)");
    clawt_lrg_form_add(form, "Directory", fields->first,
                       "On the machine the daemon runs on.");
    clawt_lrg_form_add(form, "Where it came from", fields->second, NULL);

    g_object_unref(clawt_lrg_dialog(app, "Import a skill",
                     "A directory holding a SKILL.md. It arrives disabled, "
                     "and any script beside it is left where it is -- "
                     "markdown only, on purpose.",
                     form, "Import", FALSE, on_import_response, fields,
                     g_free));
    g_object_unref(form);
}

static gboolean
on_new_response(ClawtLrgApp *app, LrgDialog *dialog, gint response,
                gpointer user_data)
{
    Fields *fields = user_data;
    g_autofree gchar *refusal = NULL;
    JsonNode *payload = clawt_lrg_skill_create_payload(
        clawt_lrg_entry_text(fields->first),
        clawt_lrg_entry_text(fields->second), &refusal);

    if (payload == NULL) {
        clawt_lrg_toast(app, refusal);
        return FALSE;
    }

    clawt_lrg_request(app, "skill.create", payload, CLAWT_LRG_REQUEST_NONE,
                      on_changed, NULL, NULL);
    return TRUE;
}

static void
on_new_clicked(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    LrgWidget *form = clawt_lrg_form_new();
    Fields *fields = g_new0(Fields, 1);

    fields->first = clawt_lrg_entry(NULL, "file-a-bug");
    fields->second = clawt_lrg_entry(NULL, "Use this when …");
    clawt_lrg_form_add(form, "Name", fields->first, NULL);
    clawt_lrg_form_add(form, "Description", fields->second, NULL);

    g_object_unref(clawt_lrg_dialog(app, "Write a skill",
                     "Lowercase letters, digits and single hyphens. The "
                     "description is the only part an agent reads before "
                     "deciding to open it, so write it as \"use this "
                     "when ...\".",
                     form, "Create", FALSE, on_new_response, fields, g_free));
    g_object_unref(form);
}

static void
on_reload_clicked(LrgButton *button, gpointer user_data)
{
    clawt_lrg_request(user_data, "skill.reload", NULL,
                      CLAWT_LRG_REQUEST_NONE, on_changed, NULL, NULL);
}

/* ── One skill, open ──────────────────────────────────────────────── */

static void
on_enable_clicked(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    const gchar *name = g_object_get_data(G_OBJECT(button), "skill");
    gboolean enable = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button),
                                                        "enable"));

    clawt_lrg_request(app, "skill.enable",
                      clawt_lrg_skill_enable_payload(name, enable),
                      CLAWT_LRG_REQUEST_NONE, on_changed, NULL, NULL);
}

static void
on_remove_confirmed(ClawtLrgApp *app, gpointer user_data)
{
    clawt_lrg_request(app, "skill.remove",
                      clawt_lrg_skill_name_payload(user_data),
                      CLAWT_LRG_REQUEST_NONE, on_changed, NULL, NULL);
}

static void
on_remove_clicked(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    const gchar *name = g_object_get_data(G_OBJECT(button), "skill");
    g_autofree gchar *body = clawt_lrg_skill_remove_prompt(name);

    /*
     * The confirmation gets its own copy of the name: the button belongs
     * to a card rebuilt on every skill.* event, which can arrive while
     * the dialog is open.
     */
    clawt_lrg_confirm(app, "Remove this skill?", body, "Remove", TRUE,
                      on_remove_confirmed, g_strdup(name), g_free);
}

static void
draw_detail(ClawtLrgApp *app)
{
    JsonObject *skill = skill_named(state.selected);
    g_autoptr(GPtrArray) notes = NULL;
    LrgWidget *content;
    LrgWidget *buttons;
    LrgWidget *toggle;
    LrgWidget *remove;
    const gchar *description;
    gboolean enabled;
    guint i;

    clawt_lrg_clear(state.detail);

    if (skill == NULL) {
        lrg_widget_set_visible(state.detail, FALSE);
        return;
    }

    lrg_widget_set_visible(state.detail, TRUE);
    content = clawt_lrg_vbox(10.0f);
    enabled = clawt_ipc_payload_boolean(skill, "enabled", FALSE);
    description = clawt_ipc_payload_string(skill, "description");

    clawt_lrg_take(content, clawt_lrg_label(state.selected, "title"));

    if (description != NULL)
        clawt_lrg_take(content, clawt_lrg_paragraph(description, "muted"));

    /*
     * Warnings above the buttons.  Placement is the whole point of them:
     * somebody here is deciding whether to enable something, and a
     * warning below the button is a warning that gets enabled around.
     */
    notes = clawt_lrg_skill_notes(skill);

    for (i = 0; i < notes->len; i++)
        clawt_lrg_take(content, note_line(g_ptr_array_index(notes, i)));

    buttons = clawt_lrg_hbox(8.0f);
    lrg_widget_set_halign(buttons, LRG_ALIGN_END);

    toggle = clawt_lrg_button(enabled ? "Disable" : "Enable",
                              enabled ? NULL : "primary",
                              G_CALLBACK(on_enable_clicked), app);
    g_object_set_data_full(G_OBJECT(toggle), "skill",
                           g_strdup(state.selected), g_free);
    g_object_set_data(G_OBJECT(toggle), "enable",
                      GINT_TO_POINTER(!enabled));
    clawt_lrg_take(buttons, toggle);

    remove = clawt_lrg_button("Remove", "danger",
                              G_CALLBACK(on_remove_clicked), app);
    g_object_set_data_full(G_OBJECT(remove), "skill",
                           g_strdup(state.selected), g_free);
    clawt_lrg_take(buttons, remove);

    clawt_lrg_take(content, buttons);

    {
        LrgWidget *card = clawt_lrg_card(content);

        lrg_widget_add_class(card, "clawt-card-selected");
        lrg_widget_set_appear(card, LRG_APPEAR_FADE, 0.0f);
        clawt_lrg_take(state.detail, card);
    }

    clawt_lrg_app_wake(app);
}

/* ── The library ──────────────────────────────────────────────────── */

static LrgWidget *
create_tile(LrgTileView *view, gpointer user_data)
{
    LrgWidget *tile = clawt_lrg_vbox(6.0f);

    lrg_container_set_padding(LRG_CONTAINER(tile), 12.0f);
    return tile;
}

/* Tiles are recycled, so a bind redraws the tile's whole content. */
static void
bind_tile(LrgTileView *view, LrgWidget *tile, GObject *item, guint position,
          gpointer user_data)
{
    JsonArray *skills = skills_array();
    guint index = GPOINTER_TO_UINT(g_object_get_data(item, SKILL_INDEX_KEY));
    JsonObject *skill;
    g_autofree gchar *caption = NULL;
    g_autofree gchar *line = NULL;

    clawt_lrg_clear(tile);

    if (skills == NULL || index >= json_array_get_length(skills))
        return;

    skill = json_array_get_object_element(skills, index);
    caption = clawt_lrg_skill_caption(skill);
    line = clawt_lrg_flow_one_line(
        clawt_ipc_payload_string(skill, "description"), 60);

    clawt_lrg_take(tile, clawt_lrg_label(
        clawt_ipc_payload_string(skill, "name"), "heading"));
    clawt_lrg_take(tile, clawt_lrg_badge(caption,
                                         clawt_lrg_skill_tone(skill)));
    clawt_lrg_take(tile, clawt_lrg_label(line, "muted"));
}

static void
on_tile_selected(LrgTileView *view, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    guint position = lrg_tile_view_get_selected(view);
    g_autoptr(GObject) item = NULL;
    JsonArray *skills = skills_array();
    guint index;

    /* a refill clears the selection; that is not somebody closing it */
    if (state.filling || position == LRG_TILE_VIEW_INVALID_POSITION)
        return;

    item = g_list_model_get_item(G_LIST_MODEL(state.items), position);

    if (item == NULL || skills == NULL)
        return;

    index = GPOINTER_TO_UINT(g_object_get_data(item, SKILL_INDEX_KEY));

    if (index >= json_array_get_length(skills))
        return;

    g_free(state.selected);
    state.selected = g_strdup(clawt_ipc_payload_string(
        json_array_get_object_element(skills, index), "name"));
    draw_detail(app);
}

static void
draw_library(ClawtLrgApp *app)
{
    JsonObject *payload = clawt_ipc_reply_object(state.skills);
    JsonArray *skills = skills_array();
    JsonArray *problems = (payload != NULL &&
                           json_object_has_member(payload, "problems"))
        ? json_object_get_array_member(payload, "problems") : NULL;
    guint reselect = LRG_TILE_VIEW_INVALID_POSITION;
    guint i;

    clawt_lrg_clear(state.problems);

    for (i = 0; problems != NULL && i < json_array_get_length(problems);
         i++) {
        LrgWidget *banner = clawt_lrg_hbox(8.0f);

        lrg_widget_add_class(banner, "clawt-banner");
        lrg_container_set_padding(LRG_CONTAINER(banner), 12.0f);
        clawt_lrg_take(banner, clawt_lrg_paragraph(
            json_array_get_string_element(problems, i), NULL));
        clawt_lrg_take(state.problems, banner);
    }

    if (skills == NULL || json_array_get_length(skills) == 0) {
        const gchar *title;
        const gchar *body;

        clawt_lrg_skills_empty(payload, &title, &body);
        lrg_widget_set_visible(LRG_WIDGET(state.tiles), FALSE);
        clawt_lrg_clear(state.library);
        clawt_lrg_take(state.library, clawt_lrg_empty_state(title, body));
        g_clear_pointer(&state.selected, g_free);
        draw_detail(app);
        return;
    }

    clawt_lrg_clear(state.library);
    lrg_widget_set_visible(LRG_WIDGET(state.tiles), TRUE);

    /*
     * Items carry an index into the reply rather than the JSON itself:
     * the reply is replaced wholesale on every refresh, and a tile bound
     * to the old one would draw freed memory.
     */
    state.filling = TRUE;
    g_list_store_remove_all(state.items);

    for (i = 0; i < json_array_get_length(skills); i++) {
        g_autoptr(GObject) item = g_object_new(G_TYPE_OBJECT, NULL);
        JsonObject *skill = json_array_get_object_element(skills, i);

        g_object_set_data(item, SKILL_INDEX_KEY, GUINT_TO_POINTER(i));
        g_list_store_append(state.items, item);

        if (g_strcmp0(clawt_ipc_payload_string(skill, "name"),
                      state.selected) == 0)
            reselect = i;
    }

    /* the open skill stays open across the refresh its own toggle caused */
    if (reselect != LRG_TILE_VIEW_INVALID_POSITION)
        lrg_tile_view_select(state.tiles, reselect);
    else
        g_clear_pointer(&state.selected, g_free);

    state.filling = FALSE;
    draw_detail(app);
}

static void
on_skill_list(ClawtLrgApp *app, JsonNode *reply, const GError *error,
              gpointer user_data)
{
    if (reply == NULL)
        return;

    g_clear_pointer(&state.skills, json_node_unref);
    state.skills = json_node_copy(reply);
    draw_library(app);
    clawt_lrg_app_wake(app);
}

/* ── Teaching a task ──────────────────────────────────────────────── */

static void
on_steps(ClawtLrgApp *app, JsonNode *reply, const GError *error,
         gpointer user_data)
{
    g_autofree gchar *text = NULL;

    if (reply == NULL)
        return;

    /*
     * Fetched only when somebody asks: teach.list answers without the
     * steps, because a demonstration can be twenty thousand of them.
     */
    text = clawt_lrg_teach_steps_text(reply);
    clawt_lrg_show_text(app, "What was recorded", NULL, text);
}

static void
on_drafted(ClawtLrgApp *app, JsonNode *reply, const GError *error,
           gpointer user_data)
{
    g_autofree gchar *draft = NULL;

    if (reply == NULL)
        return;

    clawt_lrg_toast(app, clawt_lrg_teach_done(CLAWT_LRG_TEACH_SYNTHESIZE));

    /* the draft is what has to be read before committing; show it */
    draft = clawt_lrg_teach_draft_text(reply);

    if (draft != NULL)
        clawt_lrg_show_text(app, "The drafted skill",
                            "Not a skill yet: commit it and it lands "
                            "disabled.", draft);

    refresh_skills_library(app);
}

static void
on_teach_done(ClawtLrgApp *app, JsonNode *reply, const GError *error,
              gpointer user_data)
{
    const gchar *toast;

    if (reply == NULL)
        return;

    toast = clawt_lrg_teach_done((ClawtLrgTeachAction)GPOINTER_TO_INT(
        user_data));

    if (toast != NULL)
        clawt_lrg_toast(app, toast);

    refresh_skills_library(app);
}

static void
on_recording_remove_confirmed(ClawtLrgApp *app, gpointer user_data)
{
    clawt_lrg_request(app, "teach.remove",
                      clawt_ipc_payload_new("id", user_data, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_teach_done,
                      GINT_TO_POINTER(CLAWT_LRG_TEACH_REMOVE), NULL);
}

static void
on_teach_button(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    const gchar *id = g_object_get_data(G_OBJECT(button), "recording");
    ClawtLrgTeachAction action = (ClawtLrgTeachAction)GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(button), "action"));
    gpointer done = GINT_TO_POINTER(action);

    if (id == NULL)
        return;

    /*
     * The id travels, never the widget, and each kind is written out as
     * a literal: `make parity` finds frame kinds by grepping for them.
     */
    switch (action) {
    case CLAWT_LRG_TEACH_STOP:
        clawt_lrg_request(app, "teach.stop",
                          clawt_ipc_payload_new("id", id, NULL),
                          CLAWT_LRG_REQUEST_NONE, on_teach_done, done, NULL);
        break;
    case CLAWT_LRG_TEACH_SHOW:
        clawt_lrg_request(app, "teach.show",
                          clawt_ipc_payload_new("id", id, NULL),
                          CLAWT_LRG_REQUEST_NONE, on_steps, NULL, NULL);
        break;
    case CLAWT_LRG_TEACH_SYNTHESIZE:
        clawt_lrg_toast(app, "Drafting… a model is reading the recording.");
        clawt_lrg_request(app, "teach.synthesize",
                          clawt_ipc_payload_new("id", id, NULL),
                          CLAWT_LRG_REQUEST_NONE, on_drafted, NULL, NULL);
        break;
    case CLAWT_LRG_TEACH_COMMIT:
        clawt_lrg_request(app, "teach.commit",
                          clawt_ipc_payload_new("id", id, NULL),
                          CLAWT_LRG_REQUEST_NONE, on_teach_done, done, NULL);
        break;
    case CLAWT_LRG_TEACH_REMOVE:
        clawt_lrg_confirm(app, "Remove this recording?",
                          "The trace and everything it captured are deleted.",
                          "Remove", TRUE, on_recording_remove_confirmed,
                          g_strdup(id), g_free);
        break;
    }
}

/* "Record a task": which agent, what to watch, what is being taught. */
static gboolean
on_record_response(ClawtLrgApp *app, LrgDialog *dialog, gint response,
                   gpointer user_data)
{
    Fields *fields = user_data;
    g_autofree gchar *refusal = NULL;
    JsonNode *payload = clawt_lrg_teach_start_payload(
        clawt_lrg_combo_id(fields->first), clawt_lrg_combo_id(fields->second),
        clawt_lrg_entry_text(fields->third), &refusal);

    if (payload == NULL) {
        clawt_lrg_toast(app, refusal);
        return FALSE;
    }

    clawt_lrg_request(app, "teach.start", payload, CLAWT_LRG_REQUEST_NONE,
                      on_changed, NULL, NULL);
    return TRUE;
}

static void
on_record_clicked(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    LrgWidget *form = clawt_lrg_form_new();
    Fields *fields = g_new0(Fields, 1);
    JsonObject *root = clawt_ipc_reply_object(app->agents);
    JsonArray *agents = (root != NULL && json_object_has_member(root,
                                                                "agents"))
        ? json_object_get_array_member(root, "agents") : NULL;
    g_autoptr(GPtrArray) ids = g_ptr_array_new();
    g_autoptr(GPtrArray) names = g_ptr_array_new();
    g_auto(GStrv) source_labels = NULL;
    g_auto(GStrv) sources = clawt_lrg_teach_sources(&source_labels);
    guint i;

    /*
     * The agents from the fleet as last listed, rather than a free text
     * field: a recording of an agent that does not exist is refused by
     * the daemon, and a choice cannot be misspelt.
     */
    for (i = 0; agents != NULL && i < json_array_get_length(agents); i++) {
        JsonObject *agent = json_array_get_object_element(agents, i);
        const gchar *id = clawt_ipc_payload_string(agent, "id");
        const gchar *name = clawt_ipc_payload_string(agent, "name");

        if (id == NULL)
            continue;

        g_ptr_array_add(ids, (gpointer)id);
        g_ptr_array_add(names, (gpointer)(name != NULL ? name : id));
    }

    g_ptr_array_add(ids, NULL);
    g_ptr_array_add(names, NULL);

    fields->first = clawt_lrg_combo((const gchar *const *)ids->pdata,
                                    (const gchar *const *)names->pdata,
                                    app->agent_id);

    /* walked from the library: a list here could disagree with it */
    fields->second = clawt_lrg_combo((const gchar *const *)sources,
                                     (const gchar *const *)source_labels,
                                     sources[0]);
    fields->third = clawt_lrg_entry(NULL, "What you are teaching");

    clawt_lrg_form_add(form, "Agent", fields->first, NULL);
    clawt_lrg_form_add(form, "What to watch", fields->second, NULL);
    clawt_lrg_form_add(form, "What you are teaching", fields->third, NULL);

    g_object_unref(clawt_lrg_dialog(app, "Record a task",
                     "Watching the agent captures the calls it makes. "
                     "Demonstrating captures every key you press, in any "
                     "window -- read the trace before you turn it into a "
                     "skill.",
                     form, "Record", FALSE, on_record_response, fields,
                     g_free));
    g_object_unref(form);
}

/* One recording: what it is, its buttons, and every caveat under it. */
static LrgWidget *
recording_row(ClawtLrgApp *app, ClawtLrgRecording *recording)
{
    LrgWidget *box = clawt_lrg_vbox(6.0f);
    LrgWidget *row = clawt_lrg_row(recording->title, recording->subtitle);
    const ClawtLrgTeachButton *buttons;
    guint n;
    guint i;

    if (recording->active) {
        lrg_widget_add_class(row, "clawt-busy");
        clawt_lrg_row_add_prefix(row, clawt_lrg_badge("recording", "bad"));
    }

    buttons = clawt_lrg_teach_buttons(recording->active, &n);

    for (i = 0; i < n; i++) {
        LrgWidget *button = clawt_lrg_button(buttons[i].label,
                                             buttons[i].style,
                                             G_CALLBACK(on_teach_button),
                                             app);

        g_object_set_data_full(G_OBJECT(button), "recording",
                               g_strdup(recording->id), g_free);
        g_object_set_data(G_OBJECT(button), "action",
                          GINT_TO_POINTER(buttons[i].action));
        clawt_lrg_row_add_suffix(row, button);
    }

    clawt_lrg_take(box, row);

    /*
     * The caveat under every recording, not in a document: this is where
     * somebody decides whether the trace is safe to keep, and a
     * limitation they have to go and look up is one they will not.
     */
    for (i = 0; recording->caveats != NULL && recording->caveats[i] != NULL;
         i++) {
        ClawtLrgNote note = { (gchar *)"Before you share this",
                              recording->caveats[i], "warn" };

        clawt_lrg_take(box, note_line(&note));
    }

    return box;
}

static void
on_teach_list(ClawtLrgApp *app, JsonNode *reply, const GError *error,
              gpointer user_data)
{
    g_autoptr(GPtrArray) recordings = NULL;
    guint i;

    clawt_lrg_clear(state.teach_box);

    if (reply == NULL)
        return;

    recordings = clawt_lrg_teach_recordings(reply);

    for (i = 0; i < recordings->len; i++) {
        LrgWidget *row = recording_row(app,
                                       g_ptr_array_index(recordings, i));

        clawt_lrg_appear(app, row, i);
        clawt_lrg_take(state.teach_box, row);
    }

    clawt_lrg_app_wake(app);
}

/* The recordings, as a card above the library. */
static LrgWidget *
teach_group(ClawtLrgApp *app)
{
    LrgWidget *content = clawt_lrg_vbox(10.0f);
    LrgWidget *record = clawt_lrg_button("Record a task", NULL,
                                         G_CALLBACK(on_record_clicked), app);

    clawt_lrg_take(content, clawt_lrg_label("Teach a task", "heading"));
    clawt_lrg_take(content, clawt_lrg_paragraph(
        "Record a task being done, then have a model write the procedure up "
        "as a skill. The draft lands disabled, with the same checks an "
        "imported skill gets.", "muted"));

    state.teach_box = clawt_lrg_vbox(8.0f);
    clawt_lrg_add(content, state.teach_box);
    g_object_unref(state.teach_box);

    lrg_widget_set_halign(record, LRG_ALIGN_END);
    clawt_lrg_take(content, record);

    return clawt_lrg_card(content);
}

/* ── Refreshing ───────────────────────────────────────────────────── */

static void
refresh_skills_library(ClawtLrgApp *app)
{
    if (state.page == NULL)
        return;

    clawt_lrg_request(app, "teach.list", NULL, CLAWT_LRG_QUIET,
                      on_teach_list, NULL, NULL);
    clawt_lrg_request(app, "skill.list", NULL, CLAWT_LRG_REQUEST_NONE,
                      on_skill_list, NULL, NULL);
}

/* ── The page ─────────────────────────────────────────────────────── */

static LrgWidget *
build(ClawtLrgApp *app)
{
    LrgWidget *body;
    LrgWidget *toolbar;

    state.app = app;
    state.page = clawt_lrg_page_new(
        "Skills", "Procedures your agents can open, and the recordings that "
                  "become them");
    body = clawt_lrg_page_body(state.page);
    toolbar = clawt_lrg_page_toolbar(state.page);

    clawt_lrg_take(toolbar, clawt_lrg_button("Rescan", "flat",
                                             G_CALLBACK(on_reload_clicked),
                                             app));
    clawt_lrg_take(toolbar, clawt_lrg_button("Import", NULL,
                                             G_CALLBACK(on_import_clicked),
                                             app));
    clawt_lrg_take(toolbar, clawt_lrg_button("New skill", "primary",
                                             G_CALLBACK(on_new_clicked),
                                             app));

    clawt_lrg_take(body, teach_group(app));

    state.problems = clawt_lrg_vbox(8.0f);
    clawt_lrg_add(body, state.problems);
    g_object_unref(state.problems);

    clawt_lrg_take(body, clawt_lrg_section("Library"));

    state.library = clawt_lrg_vbox(0.0f);
    clawt_lrg_add(body, state.library);
    g_object_unref(state.library);

    /*
     * A tile per skill: the grid is how a library is scanned, and the
     * one opened below it is how a skill is read.  Tiles are a fixed
     * size, so the variable-length part -- warnings, provenance -- lives
     * in the open card rather than being cut off in a tile.
     */
    state.items = g_list_store_new(G_TYPE_OBJECT);
    state.tiles = lrg_tile_view_new();
    lrg_tile_view_set_tile_size(state.tiles, 240.0f, 112.0f);
    lrg_tile_view_set_tile_spacing(state.tiles, 10.0f);
    lrg_tile_view_set_factory(state.tiles, create_tile, bind_tile, NULL,
                              NULL);
    lrg_tile_view_set_model(state.tiles, G_LIST_MODEL(state.items));
    g_signal_connect(state.tiles, "selection-changed",
                     G_CALLBACK(on_tile_selected), app);
    clawt_lrg_add(body, LRG_WIDGET(state.tiles));
    g_object_unref(state.tiles);

    state.detail = clawt_lrg_vbox(0.0f);
    lrg_widget_set_visible(state.detail, FALSE);
    clawt_lrg_add(body, state.detail);
    g_object_unref(state.detail);

    return state.page;
}

static void
refresh(ClawtLrgApp *app)
{
    refresh_skills_library(app);
}

static void
event(ClawtLrgApp *app, ClawtEvent *event)
{
    const gchar *kind = clawt_event_get_kind(event);

    /*
     * A skill enabled, imported or removed elsewhere -- the CLI, another
     * window -- and a recording started or stopped, are both this page's
     * to redraw, and the ones that arrive from somewhere else are exactly
     * when nothing on screen would have prompted it.
     */
    if (state.page == NULL || kind == NULL)
        return;

    /* a recording publishes as it goes; re-read only where it is seen */
    if (app->page != CLAWT_PAGE_SKILLS)
        return;

    if (g_str_has_prefix(kind, "skill.") || g_str_has_prefix(kind, "teach."))
        refresh_skills_library(app);
}

static const ClawtLrgPage page_impl = {
    CLAWT_PAGE_SKILLS, build, refresh, event
};

const ClawtLrgPage *
clawt_lrg_skills_page(void)
{
    return &page_impl;
}
