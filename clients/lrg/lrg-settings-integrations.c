/*
 * lrg-settings-integrations.c - Settings: Integrations
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The integrations that give an agent a channel or tools, the editor for
 * one, and the Matrix sign-in and room picker two of them need.  Mirrors
 * clients/gtk/gtk-prefs-integrations.c.
 *
 * The form is generated, never written: every control comes from
 * clawt_integration_fields(), the table the GTK and web editors draw
 * from too, and the save walks the same table with the same
 * applies-predicate (clawt_lrg_settings_integration_payload()) -- so a
 * field that is not shown is not saved, and the two halves cannot come
 * to disagree about which fields exist, which is what GTK's two
 * hand-written chains once did.
 *
 * Every control is kept by its *slot* (clawt_lrg_settings_field_slot():
 * the key, or the label for a conditional field, since notify's `url`
 * is two fields) and read back as text, which is what the model takes.
 */

#include "clawt-lrg.h"
#include "clawt-lrg-settings-model.h"

#include <string.h>

typedef struct {
    LrgWidget *list;
    guint      generation;
} Integrations;

static Integrations integrations;

static void refresh(ClawtLrgApp *app);
static void open_editor(ClawtLrgApp *app, const gchar *name,
                        const gchar *type_id);

/* Holds a weak pointer to a dialog for a reply that may outlive it. */
static void
weak_dialog_free(gpointer data)
{
    LrgDialog **dialog = data;

    g_clear_weak_pointer(dialog);
    g_free(dialog);
}

static LrgDialog **
weak_dialog_new(LrgDialog *dialog)
{
    LrgDialog **weak = g_new0(LrgDialog *, 1);

    g_set_weak_pointer(weak, dialog);
    return weak;
}

/* ── The editor ───────────────────────────────────────────────────── */

typedef struct {
    ClawtLrgApp *app;
    gchar       *name;
    gchar       *type_id;
    LrgDialog   *dialog;        /* weak; every widget below is its */
    LrgWidget   *enabled;
    LrgWidget   *description;
    LrgWidget   *scope;
    GPtrArray   *agent_switches;
    GHashTable  *controls;      /* slot -> control */
    GHashTable  *fields;        /* slot -> the field box, for hiding */
} Editor;

static void
editor_clear(gpointer data)
{
    Editor *editor = data;

    g_clear_weak_pointer(&editor->dialog);
    g_free(editor->name);
    g_free(editor->type_id);
    g_clear_pointer(&editor->agent_switches, g_ptr_array_unref);
    g_clear_pointer(&editor->controls, g_hash_table_unref);
    g_clear_pointer(&editor->fields, g_hash_table_unref);
}

/*
 * Counted: the dialog holds one and every request in flight one more,
 * because closing the dialog mid-save finalises it before the reply
 * comes back to read this.  A reply checks editor->dialog before it
 * touches a widget, since the widgets went with the dialog.
 */
static void
editor_release(gpointer data)
{
    g_rc_box_release_full(data, editor_clear);
}

/* Every control's value as text, keyed by slot. */
static GHashTable *
collect_values(Editor *editor)
{
    GHashTable *values = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, g_free);
    GHashTableIter iter;
    gpointer key;
    gpointer value;

    g_hash_table_iter_init(&iter, editor->controls);

    while (g_hash_table_iter_next(&iter, &key, &value)) {
        LrgWidget *control = value;
        gchar *text;

        if (LRG_IS_CHECKBOX(control))
            text = g_strdup(clawt_lrg_switch_active(control) ? "true"
                                                             : "false");
        else if (LRG_IS_COMBO_BOX(control))
            text = g_strdup(clawt_lrg_combo_id(control));
        else if (LRG_IS_TEXT_INPUT(control))
            text = g_strdup(clawt_lrg_entry_text(control));
        else
            continue;

        g_hash_table_insert(values, g_strdup(key),
                            (text != NULL) ? text : g_strdup(""));
    }

    return values;
}

/*
 * Hides what does not apply to what is chosen.  A field that does not
 * apply reads as something not filled in, which is worse than a missing
 * one.  The save asks the same predicate, not the widgets' visibility.
 */
static void
apply_visibility(Editor *editor)
{
    g_autoptr(GHashTable) values = collect_values(editor);
    const ClawtIntegrationField *list;
    gsize n = 0;
    gsize i;

    list = clawt_integration_fields(editor->type_id, &n);

    for (i = 0; i < n; i++) {
        const gchar *when = clawt_lrg_settings_when_value(
            editor->type_id, values, list[i].when_key);
        gboolean applies = clawt_integration_field_applies(&list[i], when);
        LrgWidget *box;

        if (list[i].kind == CLAWT_FIELD_FLAGS) {
            guint c;

            for (c = 0; list[i].choices != NULL &&
                        list[i].choices[c] != NULL; c++) {
                g_autofree gchar *slot =
                    clawt_lrg_settings_flag_slot(&list[i], list[i].choices[c]);

                box = g_hash_table_lookup(editor->fields, slot);

                if (box != NULL)
                    lrg_widget_set_visible(box, applies);
            }

            continue;
        }

        box = g_hash_table_lookup(editor->fields,
                                  clawt_lrg_settings_field_slot(&list[i]));

        if (box != NULL)
            lrg_widget_set_visible(box, applies);
    }

    clawt_lrg_app_wake(editor->app);
}

static void
on_choice_changed(LrgComboBox *combo, gpointer user_data)
{
    apply_visibility(user_data);
}

/* Adds @control under @label and records it by @slot. */
static void
add_control(Editor *editor, LrgWidget *form, const gchar *slot,
            const gchar *label, LrgWidget *control, LrgWidget *shown,
            const gchar *hint)
{
    g_hash_table_replace(editor->controls, g_strdup(slot), control);
    clawt_lrg_form_add(form, label, shown, hint);

    /* the field box form_add made: what hides the label and hint too */
    g_hash_table_replace(editor->fields, g_strdup(slot),
                         LRG_WIDGET(lrg_widget_get_parent(shown)));
}

/* ── Matrix sign-in ───────────────────────────────────────────────── */

typedef struct {
    Editor    *editor;      /* a reference */
    LrgWidget *user;
    LrgWidget *password;
    LrgDialog *dialog;      /* weak */
} SignIn;

static void
sign_in_clear(gpointer data)
{
    SignIn *sign_in = data;

    g_clear_weak_pointer(&sign_in->dialog);
    editor_release(sign_in->editor);
}

static void
sign_in_release(gpointer data)
{
    g_rc_box_release_full(data, sign_in_clear);
}

static void
on_signed_in(ClawtLrgApp *app, JsonNode *reply, const GError *error,
             gpointer user_data)
{
    SignIn *sign_in = user_data;
    Editor *editor = sign_in->editor;
    JsonObject *root = clawt_ipc_reply_object(reply);
    const gchar *path;
    g_autofree gchar *text = NULL;

    if (root == NULL)
        return;

    /* the daemon wrote the user id itself; the form shows it */
    if (editor->dialog != NULL) {
        LrgWidget *user_id = g_hash_table_lookup(editor->controls, "user_id");
        const gchar *value = clawt_ipc_payload_string(root, "user_id");

        if (user_id != NULL && LRG_IS_TEXT_INPUT(user_id) && value != NULL)
            lrg_text_input_set_text(LRG_TEXT_INPUT(user_id), value);
    }

    /*
     * Where the token went, never the token: the daemon wrote it 0600
     * and reports only the path, which is what a person needs to find
     * it or delete it.
     */
    path = clawt_ipc_payload_string(root, "token_file");
    text = (path != NULL)
        ? g_strdup_printf("Signed in. The token is on the daemon's disk, at "
                          "%s -- not here.", path)
        : g_strdup("Signed in. The token is on the daemon's disk, not here.");
    clawt_lrg_toast(app, text);

    if (sign_in->dialog != NULL)
        lrg_dialog_close(sign_in->dialog);
}

static gboolean
on_sign_in_response(ClawtLrgApp *app, LrgDialog *dialog, gint response,
                    gpointer user_data)
{
    SignIn *sign_in = user_data;
    Editor *editor = sign_in->editor;
    LrgWidget *homeserver = (editor->dialog != NULL)
        ? g_hash_table_lookup(editor->controls, "homeserver") : NULL;
    g_autofree gchar *password =
        g_strdup(clawt_lrg_entry_text(sign_in->password));
    const gchar *user = clawt_lrg_entry_text(sign_in->user);

    /*
     * Cleared whatever happens: a wrong password leaves the dialog open
     * to retype, and leaving the old one there sends it again by
     * accident.  It crosses the socket once and is never stored.
     */
    lrg_text_input_set_text(LRG_TEXT_INPUT(sign_in->password), "");

    if (*user == '\0' || *password == '\0') {
        clawt_lrg_toast(app, "A user and a password, both.");
        return FALSE;
    }

    clawt_lrg_request(app, "integration.matrix_login",
                      clawt_ipc_payload_new(
                          "integration", editor->name,
                          "homeserver", (homeserver != NULL)
                              ? clawt_lrg_entry_text(homeserver) : NULL,
                          "user", user,
                          "password", password,
                          NULL),
                      CLAWT_LRG_REQUEST_NONE, on_signed_in,
                      g_rc_box_acquire(sign_in), sign_in_release);

    memset(password, 0, strlen(password));
    return FALSE;
}

static void
on_sign_in_clicked(LrgButton *button, gpointer user_data)
{
    Editor *editor = user_data;
    SignIn *sign_in = g_rc_box_new0(SignIn);
    LrgWidget *form = clawt_lrg_form_new();
    LrgDialog *dialog;

    sign_in->editor = g_rc_box_acquire(editor);
    sign_in->user = clawt_lrg_entry(NULL, "@agent:example.org");
    sign_in->password = clawt_lrg_entry(NULL, "Password");
    lrg_text_input_set_password_mode(LRG_TEXT_INPUT(sign_in->password),
                                     TRUE);

    clawt_lrg_form_add(form, "User", sign_in->user, NULL);
    clawt_lrg_form_add(form, "Password", sign_in->password, NULL);
    lrg_widget_set_min_size(form, 440.0f * (gfloat)editor->app->scale, 0.0f);

    dialog = clawt_lrg_dialog(
        editor->app, "Sign in to Matrix",
        "The password is used once, by the daemon, and is never stored. "
        "What comes back is an access token, written to a file only the "
        "daemon can read. It appears on your account's device list as "
        "\"clawtilla\", which is where you revoke it.",
        form, "Sign in", FALSE, on_sign_in_response, sign_in,
        sign_in_release);
    g_set_weak_pointer(&sign_in->dialog, dialog);
    g_object_unref(dialog);
    g_object_unref(form);
}

/* ── Choosing rooms ───────────────────────────────────────────────── */

typedef struct {
    Editor    *editor;      /* a reference */
    GPtrArray *switches;    /* unowned: the dialog's */
} RoomPicker;

static void
room_picker_free(gpointer data)
{
    RoomPicker *picker = data;

    editor_release(picker->editor);
    g_ptr_array_unref(picker->switches);
    g_free(picker);
}

static gboolean
on_rooms_chosen(ClawtLrgApp *app, LrgDialog *dialog, gint response,
                gpointer user_data)
{
    RoomPicker *picker = user_data;
    LrgWidget *rooms;
    g_autoptr(GString) chosen = g_string_new(NULL);
    guint i;

    if (picker->editor->dialog == NULL)
        return TRUE;

    rooms = g_hash_table_lookup(picker->editor->controls, "rooms");

    for (i = 0; i < picker->switches->len; i++) {
        LrgWidget *one = g_ptr_array_index(picker->switches, i);

        if (!clawt_lrg_switch_active(one))
            continue;

        if (chosen->len > 0)
            g_string_append(chosen, ", ");

        g_string_append(chosen, g_object_get_data(G_OBJECT(one),
                                                  "clawt-room"));
    }

    if (rooms != NULL && LRG_IS_TEXT_INPUT(rooms))
        lrg_text_input_set_text(LRG_TEXT_INPUT(rooms), chosen->str);

    return TRUE;
}

static void
on_rooms(ClawtLrgApp *app, JsonNode *reply, const GError *error,
         gpointer user_data)
{
    Editor *editor = user_data;
    JsonArray *rooms = clawt_lrg_settings_member_array(
        clawt_ipc_reply_object(reply), "rooms");
    LrgWidget *box = clawt_lrg_vbox(6.0f);
    LrgScrollBox *scroll;
    RoomPicker *picker;
    LrgWidget *current_entry;
    g_auto(GStrv) current = NULL;
    guint i;

    if (reply == NULL || editor->dialog == NULL) {
        g_object_unref(box);
        return;
    }

    current_entry = g_hash_table_lookup(editor->controls, "rooms");
    current = clawt_lrg_settings_split_list(
        (current_entry != NULL) ? clawt_lrg_entry_text(current_entry) : NULL);

    picker = g_new0(RoomPicker, 1);
    picker->editor = g_rc_box_acquire(editor);
    picker->switches = g_ptr_array_new();

    for (i = 0; rooms != NULL && i < json_array_get_length(rooms); i++) {
        JsonObject *room = json_array_get_object_element(rooms, i);
        const gchar *id = clawt_ipc_payload_string(room, "id");
        const gchar *label = clawt_ipc_payload_string(room, "label");
        g_autofree gchar *text = NULL;
        LrgWidget *one;

        if (id == NULL)
            continue;

        text = g_strdup_printf("%s  —  %s",
                               (label != NULL) ? label : id, id);
        one = clawt_lrg_switch(text, g_strv_contains(
                                         (const gchar *const *)current, id));
        g_object_set_data_full(G_OBJECT(one), "clawt-room", g_strdup(id),
                               g_free);
        g_ptr_array_add(picker->switches, one);
        clawt_lrg_take(box, one);
    }

    if (picker->switches->len == 0)
        clawt_lrg_take(box, clawt_lrg_paragraph(
            "That account is not in any rooms yet. Invite it from your "
            "Matrix client and look again.", "muted"));

    scroll = lrg_scroll_box_new();
    lrg_scroll_box_set_hscroll(scroll, FALSE);
    lrg_scroll_box_set_vscroll(scroll, TRUE);
    lrg_scroll_box_set_child(scroll, box);
    lrg_widget_set_min_size(LRG_WIDGET(scroll), 520.0f * (gfloat)app->scale,
                            360.0f * (gfloat)app->scale);
    g_object_unref(box);

    g_object_unref(clawt_lrg_dialog(
        app, "Listen in",
        "Choose none to listen in every room this account is in, "
        "including ones it is invited to later.",
        LRG_WIDGET(scroll), "Use these", FALSE, on_rooms_chosen, picker,
        room_picker_free));
    g_object_unref(scroll);
}

static void
on_choose_rooms(LrgButton *button, gpointer user_data)
{
    Editor *editor = user_data;

    clawt_lrg_request(editor->app, "integration.matrix_rooms",
                      clawt_ipc_payload_new("integration", editor->name,
                                            NULL),
                      CLAWT_LRG_REQUEST_NONE, on_rooms,
                      g_rc_box_acquire(editor), editor_release);
}

/* ── Save, check, remove ──────────────────────────────────────────── */

static void
on_saved(ClawtLrgApp *app, JsonNode *reply, const GError *error,
         gpointer user_data)
{
    Editor *editor = user_data;

    if (reply == NULL)
        return;

    clawt_lrg_toast(app, "Saved.");

    if (editor->dialog != NULL)
        lrg_dialog_close(editor->dialog);

    /* an integration binds to agents, so the selected agent's page too */
    refresh(app);
    clawt_lrg_show_page(app, app->page);
}

static gboolean
on_save(ClawtLrgApp *app, LrgDialog *dialog, gint response,
        gpointer user_data)
{
    Editor *editor = user_data;
    g_autoptr(GHashTable) values = collect_values(editor);
    g_autoptr(GPtrArray) agents = g_ptr_array_new();
    g_autoptr(GError) error = NULL;
    ClawtLrgIntegrationForm form = { 0 };
    JsonNode *payload;
    guint i;

    /*
     * The ticked agents go up whatever the scope, so switching to
     * everyone and back does not lose the choice somebody made.
     */
    for (i = 0; i < editor->agent_switches->len; i++) {
        LrgWidget *one = g_ptr_array_index(editor->agent_switches, i);

        if (clawt_lrg_switch_active(one))
            g_ptr_array_add(agents, g_object_get_data(G_OBJECT(one),
                                                      "clawt-agent"));
    }

    g_ptr_array_add(agents, NULL);

    form.name = editor->name;
    form.type_id = editor->type_id;
    form.enabled = clawt_lrg_switch_active(editor->enabled);
    form.description = clawt_lrg_entry_text(editor->description);
    form.scope = clawt_lrg_combo_id(editor->scope);
    form.agents = (const gchar *const *)agents->pdata;
    form.values = values;

    payload = clawt_lrg_settings_integration_payload(&form, &error);

    if (payload == NULL) {
        clawt_lrg_toast(app, error->message);
        return FALSE;
    }

    clawt_lrg_request(app, "integration.update", payload,
                      CLAWT_LRG_REQUEST_NONE, on_saved,
                      g_rc_box_acquire(editor), editor_release);

    /* closed by the reply, with the form intact if it is refused */
    return FALSE;
}

static void
on_health(ClawtLrgApp *app, JsonNode *reply, const GError *error,
          gpointer user_data)
{
    JsonArray *checks = clawt_lrg_settings_member_array(
        clawt_ipc_reply_object(reply), "checks");
    guint i;

    for (i = 0; checks != NULL && i < json_array_get_length(checks); i++)
        clawt_lrg_toast(app, clawt_lrg_settings_health_text(
                                 json_array_get_object_element(checks, i)));
}

/*
 * A health check belongs to a binding, not an instance -- one instance
 * may be reachable for one agent and not another -- so it is asked as
 * the first agent in scope, which is the one that will notice first.
 */
static void
on_check_list(ClawtLrgApp *app, JsonNode *reply, const GError *error,
              gpointer user_data)
{
    Editor *editor = user_data;
    JsonObject *integration =
        clawt_lrg_settings_find_integration(reply, editor->name);
    JsonArray *effective =
        clawt_lrg_settings_member_array(integration, "effective_agents");

    if (reply == NULL)
        return;

    if (effective == NULL || json_array_get_length(effective) == 0) {
        clawt_lrg_toast(app, "No agent has this yet, so there is nothing "
                             "to check it as.");
        return;
    }

    clawt_lrg_request(app, "integration.health",
                      clawt_ipc_payload_new(
                          "agent", json_array_get_string_element(effective, 0),
                          "integration", editor->name, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_health, NULL, NULL);
}

static void
on_test_sent(ClawtLrgApp *app, JsonNode *reply, const GError *error,
             gpointer user_data)
{
    if (reply != NULL)
        clawt_lrg_toast(app, "Sent. If nothing arrived, it is not reaching "
                             "you.");
}

static void
on_check_clicked(LrgButton *button, gpointer user_data)
{
    Editor *editor = user_data;

    /*
     * A notifier has nothing to connect to: the only honest test is to
     * send one.  The daemon ignores the event list and quiet hours for
     * it, because a button that did nothing at half past eleven would
     * be indistinguishable from a broken one.
     */
    if (g_strcmp0(editor->type_id, "notify") == 0) {
        clawt_lrg_request(editor->app, "integration.notify_test",
                          clawt_ipc_payload_new("integration", editor->name,
                                                NULL),
                          CLAWT_LRG_REQUEST_NONE, on_test_sent, NULL, NULL);
        return;
    }

    clawt_lrg_request(editor->app, "integration.list", NULL,
                      CLAWT_LRG_REQUEST_NONE, on_check_list,
                      g_rc_box_acquire(editor), editor_release);
}

static void
on_removed(ClawtLrgApp *app, JsonNode *reply, const GError *error,
           gpointer user_data)
{
    LrgDialog **dialog = user_data;

    if (reply == NULL)
        return;

    clawt_lrg_toast(app, "Removed. Any credential file it wrote is still "
                         "on disk.");

    if (*dialog != NULL)
        lrg_dialog_close(*dialog);

    refresh(app);
    clawt_lrg_show_page(app, app->page);
}

typedef struct {
    gchar     *name;
    LrgDialog *editor;    /* weak */
} Removal;

static void
removal_free(gpointer data)
{
    Removal *removal = data;

    g_clear_weak_pointer(&removal->editor);
    g_free(removal->name);
    g_free(removal);
}

static void
on_remove_confirmed(ClawtLrgApp *app, gpointer user_data)
{
    Removal *removal = user_data;

    clawt_lrg_request(app, "integration.remove",
                      clawt_ipc_payload_new("name", removal->name, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_removed,
                      weak_dialog_new(removal->editor), weak_dialog_free);
}

static void
on_remove_clicked(LrgButton *button, gpointer user_data)
{
    Editor *editor = user_data;
    Removal *removal = g_new0(Removal, 1);
    g_autofree gchar *title = g_strdup_printf("Remove %s?", editor->name);

    removal->name = g_strdup(editor->name);
    g_set_weak_pointer(&removal->editor, editor->dialog);

    clawt_lrg_confirm(editor->app, title,
                      "Every agent it reaches loses it. A credential file it "
                      "wrote stays on disk.",
                      "Remove", TRUE, on_remove_confirmed, removal,
                      removal_free);
}

/* ── Building the editor ──────────────────────────────────────────── */

/* One control per field, or one per choice for a flags field. */
static void
build_type_fields(Editor *editor, LrgWidget *form, JsonObject *integration)
{
    const ClawtIntegrationField *list;
    gsize n = 0;
    gsize i;

    list = clawt_integration_fields(editor->type_id, &n);

    for (i = 0; i < n; i++) {
        const ClawtIntegrationField *field = &list[i];
        const gchar *slot = clawt_lrg_settings_field_slot(field);
        g_autofree gchar *initial = NULL;
        LrgWidget *control = NULL;
        LrgWidget *shown;

        if (field->kind == CLAWT_FIELD_FLAGS) {
            guint c;

            clawt_lrg_take(form, clawt_lrg_label(field->label, "caption"));

            for (c = 0; field->choices[c] != NULL; c++) {
                g_autofree gchar *flag =
                    clawt_lrg_settings_flag_slot(field, field->choices[c]);
                LrgWidget *one = clawt_lrg_switch(
                    field->choice_labels[c],
                    clawt_lrg_settings_flag_initial(field, field->choices[c],
                                                    integration));

                add_control(editor, form, flag, NULL, one, one, NULL);
            }

            continue;
        }

        initial = clawt_lrg_settings_field_initial(field, integration);

        switch (field->kind) {
        case CLAWT_FIELD_BOOLEAN:
            control = clawt_lrg_switch(field->label,
                                       g_strcmp0(initial, "true") == 0);
            add_control(editor, form, slot, NULL, control, control,
                        field->hint);
            continue;

        case CLAWT_FIELD_CHOICE:
            /* the model's choices, the selection the instance has */
            control = clawt_lrg_combo(field->choices, field->choice_labels,
                                      initial);
            g_signal_connect(control, "changed",
                             G_CALLBACK(on_choice_changed), editor);
            add_control(editor, form, slot, field->label, control, control,
                        field->hint);
            continue;

        case CLAWT_FIELD_SECRET:
            /* never the value: what goes in is a reference to one */
            control = clawt_lrg_entry(NULL, (field->example != NULL)
                                                ? field->example
                                                : "env:NAME or file:PATH");
            break;

        case CLAWT_FIELD_INT:
        case CLAWT_FIELD_LIST:
        case CLAWT_FIELD_TEXT:
        case CLAWT_FIELD_FLAGS:
            control = clawt_lrg_entry(initial, field->example);
            break;
        }

        /* seeded NULL for -Wmaybe-uninitialized; the switch names all */
        if (control == NULL)
            continue;

        shown = control;

        /* the two Matrix conveniences hang off their own fields */
        if (g_strcmp0(editor->type_id, "matrix") == 0 &&
            field->when_key == NULL &&
            (g_strcmp0(field->key, "homeserver") == 0 ||
             g_strcmp0(field->key, "rooms") == 0)) {
            gboolean is_home = g_strcmp0(field->key, "homeserver") == 0;

            shown = clawt_lrg_hbox(8.0f);
            lrg_widget_set_fill_weight(control, 1.0f);
            clawt_lrg_take(shown, control);
            clawt_lrg_take(shown, clawt_lrg_button(
                is_home ? "Sign in…" : "Choose…", NULL,
                is_home ? G_CALLBACK(on_sign_in_clicked)
                        : G_CALLBACK(on_choose_rooms),
                editor));
        }

        add_control(editor, form, slot, field->label, control, shown,
                    field->hint);
    }
}

/*
 * One switch per agent: the question is per agent -- does the
 * researcher get this -- and a multi-select list makes it a gesture
 * with a hidden state instead of a yes or a no.
 */
static LrgWidget *
build_agent_group(Editor *editor, JsonObject *integration)
{
    LrgWidget *box = clawt_lrg_vbox(6.0f);
    JsonArray *agents = clawt_lrg_settings_member_array(
        clawt_ipc_reply_object(editor->app->agents), "agents");
    g_autofree gchar *joined =
        clawt_lrg_settings_join_strings(integration, "agents", ",");
    g_auto(GStrv) chosen = clawt_lrg_settings_split_list(joined);
    guint i;

    clawt_lrg_take(box, clawt_lrg_label("Which agents", "heading"));
    clawt_lrg_take(box, clawt_lrg_paragraph(
        "Used when the scope above is \"These agents\". Kept either way, "
        "so switching to everyone and back does not lose the choice.",
        "muted"));

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
        one = clawt_lrg_switch(text, g_strv_contains(
                                         (const gchar *const *)chosen, id));
        g_object_set_data_full(G_OBJECT(one), "clawt-agent", g_strdup(id),
                               g_free);
        g_ptr_array_add(editor->agent_switches, one);
        clawt_lrg_take(box, one);
    }

    return box;
}

typedef struct {
    gchar *name;
    gchar *type_id;
} EditorRequest;

static void
editor_request_free(gpointer data)
{
    EditorRequest *request = data;

    g_free(request->name);
    g_free(request->type_id);
    g_free(request);
}

/*
 * The instance as the daemon has it now, never a copy kept from the
 * list: signing in changes it behind the dialog's back, and a dialog
 * showing what it had before would save the old values over the new.
 */
static void
on_editor_list(ClawtLrgApp *app, JsonNode *reply, const GError *error,
               gpointer user_data)
{
    EditorRequest *request = user_data;
    JsonObject *integration =
        clawt_lrg_settings_find_integration(reply, request->name);
    Editor *editor;
    LrgWidget *content = clawt_lrg_vbox(16.0f);
    LrgWidget *form = clawt_lrg_form_new();
    LrgWidget *actions = clawt_lrg_hbox(8.0f);
    LrgScrollBox *scroll;
    LrgDialog *dialog;
    const gchar *type;
    const gchar *summary;
    g_autoptr(GPtrArray) scope_ids = g_ptr_array_new();
    g_autoptr(GPtrArray) scope_labels = g_ptr_array_new();
    guint i;

    /*
     * Gone since the list was drawn -- removed from the CLI, say.  An
     * editor for it would save a new one under the old name.
     */
    if (reply != NULL && integration == NULL && request->type_id == NULL) {
        g_autofree gchar *text = g_strdup_printf(
            "%s is not there any more.", request->name);

        clawt_lrg_toast(app, text);
        refresh(app);
    }

    if (reply == NULL || (integration == NULL && request->type_id == NULL)) {
        g_object_unref(content);
        g_object_unref(form);
        g_object_unref(actions);
        return;
    }

    type = clawt_ipc_payload_string(integration, "type");
    summary = clawt_ipc_payload_string(integration, "summary");

    editor = g_rc_box_new0(Editor);
    editor->app = app;
    editor->name = g_strdup(request->name);
    editor->type_id = g_strdup((type != NULL) ? type : request->type_id);
    editor->agent_switches = g_ptr_array_new();
    editor->controls = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                             NULL);
    editor->fields = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                           NULL);

    clawt_lrg_take(content, clawt_lrg_label(
        clawt_integration_type_label(editor->type_id), "heading"));

    if (summary != NULL && *summary != '\0')
        clawt_lrg_take(content, clawt_lrg_paragraph(summary, "muted"));

    editor->enabled = clawt_lrg_switch(
        "Enabled", clawt_ipc_payload_boolean(integration, "enabled", TRUE));
    clawt_lrg_form_add(form, NULL, editor->enabled, NULL);

    editor->description = clawt_lrg_entry(
        clawt_ipc_payload_string(integration, "description"),
        "What it is for");
    clawt_lrg_form_add(form, "What it is for", editor->description,
                       "Written into every agent's TOOLS.org");

    /* who gets it, walked from ClawtScope */
    for (i = 0; i < clawt_lrg_settings_scope_count(); i++) {
        const gchar *nick = clawt_lrg_settings_scope_nth_nick(i);

        g_ptr_array_add(scope_ids, (gpointer)nick);
        g_ptr_array_add(scope_labels,
                        (gpointer)clawt_lrg_settings_scope_label(nick));
    }

    g_ptr_array_add(scope_ids, NULL);
    g_ptr_array_add(scope_labels, NULL);

    {
        const gchar *scope = clawt_ipc_payload_string(integration, "scope");

        editor->scope = clawt_lrg_combo(
            (const gchar *const *)scope_ids->pdata,
            (const gchar *const *)scope_labels->pdata,
            (scope != NULL) ? scope : "selected");
        clawt_lrg_form_add(form, "Who gets it", editor->scope, NULL);
    }

    build_type_fields(editor, form, integration);
    clawt_lrg_take(content, form);
    clawt_lrg_take(content, build_agent_group(editor, integration));

    clawt_lrg_take(actions, clawt_lrg_button(
        g_strcmp0(editor->type_id, "notify") == 0 ? "Send a test" : "Check",
        NULL, G_CALLBACK(on_check_clicked), editor));
    clawt_lrg_take(actions, clawt_lrg_button("Remove", "danger",
                                             G_CALLBACK(on_remove_clicked),
                                             editor));
    clawt_lrg_take(content, actions);

    scroll = lrg_scroll_box_new();
    lrg_scroll_box_set_hscroll(scroll, FALSE);
    lrg_scroll_box_set_vscroll(scroll, TRUE);
    lrg_scroll_box_set_kinetic(scroll, TRUE);
    lrg_scroll_box_set_child(scroll, content);
    lrg_widget_set_min_size(LRG_WIDGET(scroll), 600.0f * (gfloat)app->scale,
                            520.0f * (gfloat)app->scale);
    g_object_unref(content);

    apply_visibility(editor);

    dialog = clawt_lrg_dialog(app, editor->name, NULL, LRG_WIDGET(scroll),
                              "Save", FALSE, on_save, editor,
                              editor_release);
    g_set_weak_pointer(&editor->dialog, dialog);
    g_object_unref(dialog);
    g_object_unref(scroll);
}

static void
open_editor(ClawtLrgApp *app, const gchar *name, const gchar *type_id)
{
    EditorRequest *request = g_new0(EditorRequest, 1);

    request->name = g_strdup(name);
    request->type_id = g_strdup(type_id);

    clawt_lrg_request(app, "integration.list", NULL, CLAWT_LRG_REQUEST_NONE,
                      on_editor_list, request, editor_request_free);
}

static void
on_edit_clicked(LrgButton *button, gpointer user_data)
{
    open_editor(user_data,
                g_object_get_data(G_OBJECT(button), "clawt-integration"),
                NULL);
}

/* ── Adding one ───────────────────────────────────────────────────── */

typedef struct {
    LrgWidget *name;
    LrgWidget *type;
    LrgWidget *summary;
    GPtrArray *summaries;   /* parallel to the combo's ids */
    GPtrArray *types;
    LrgDialog *dialog;      /* weak */
    gchar     *chosen_name;
    gchar     *chosen_type;
} AddIntegration;

static void
add_clear(gpointer data)
{
    AddIntegration *add = data;

    g_clear_weak_pointer(&add->dialog);
    g_ptr_array_unref(add->summaries);
    g_ptr_array_unref(add->types);
    g_free(add->chosen_name);
    g_free(add->chosen_type);
}

static void
add_release(gpointer data)
{
    g_rc_box_release_full(data, add_clear);
}

/* What the chosen type does and needs, under the picker. */
static void
on_add_type_changed(LrgComboBox *combo, gpointer user_data)
{
    AddIntegration *add = user_data;
    const gchar *id = clawt_lrg_combo_id(add->type);
    g_autofree gchar *escaped = NULL;
    guint i;

    for (i = 0; id != NULL && i < add->types->len; i++) {
        if (g_strcmp0(g_ptr_array_index(add->types, i), id) != 0)
            continue;

        escaped = clawt_lrg_markdown_escape(
            g_ptr_array_index(add->summaries, i));
        lrg_markdown_label_set_markdown(LRG_MARKDOWN_LABEL(add->summary),
                                        escaped);
        break;
    }
}

static void
on_added(ClawtLrgApp *app, JsonNode *reply, const GError *error,
         gpointer user_data)
{
    AddIntegration *add = user_data;

    if (reply == NULL)
        return;

    if (add->dialog != NULL)
        lrg_dialog_close(add->dialog);

    refresh(app);

    /*
     * Straight into the editor: one with a name and a type reaches
     * nobody and does nothing, so stopping here is stopping halfway.
     */
    open_editor(app, add->chosen_name, add->chosen_type);
}

static gboolean
on_add_response(ClawtLrgApp *app, LrgDialog *dialog, gint response,
                gpointer user_data)
{
    AddIntegration *add = user_data;
    g_autofree gchar *name = g_strdup(clawt_lrg_entry_text(add->name));
    const gchar *type_id = clawt_lrg_combo_id(add->type);

    g_strstrip(name);

    if (*name == '\0') {
        clawt_lrg_toast(app, "It needs a name.");
        return FALSE;
    }

    if (type_id == NULL)
        return FALSE;

    g_free(add->chosen_name);
    g_free(add->chosen_type);
    add->chosen_name = g_strdup(name);
    add->chosen_type = g_strdup(type_id);

    clawt_lrg_request(app, "integration.add",
                      clawt_ipc_payload_new("name", name, "type", type_id,
                                            NULL),
                      CLAWT_LRG_REQUEST_NONE, on_added,
                      g_rc_box_acquire(add), add_release);

    return FALSE;
}

static void
on_types(ClawtLrgApp *app, JsonNode *reply, const GError *error,
         gpointer user_data)
{
    JsonArray *types = clawt_lrg_settings_member_array(
        clawt_ipc_reply_object(reply), "types");
    AddIntegration *add;
    LrgWidget *form;
    g_autoptr(GPtrArray) labels = NULL;
    LrgDialog *dialog;
    guint i;

    if (reply == NULL)
        return;

    add = g_rc_box_new0(AddIntegration);
    add->types = g_ptr_array_new_with_free_func(g_free);
    add->summaries = g_ptr_array_new_with_free_func(g_free);
    labels = g_ptr_array_new();

    for (i = 0; types != NULL && i < json_array_get_length(types); i++) {
        JsonObject *type = json_array_get_object_element(types, i);
        const gchar *id = clawt_ipc_payload_string(type, "id");

        if (id == NULL)
            continue;

        /* the name a person would say, not the config value */
        g_ptr_array_add(add->types, g_strdup(id));
        g_ptr_array_add(labels, (gpointer)clawt_integration_type_label(id));
        g_ptr_array_add(add->summaries,
                        clawt_lrg_settings_integration_type_text(
                            id, clawt_ipc_payload_string(type, "summary")));
    }

    g_ptr_array_add(add->types, NULL);
    g_ptr_array_add(labels, NULL);

    form = clawt_lrg_form_new();
    add->name = clawt_lrg_entry(NULL, "matrix-home");
    add->type = clawt_lrg_combo((const gchar *const *)add->types->pdata,
                                (const gchar *const *)labels->pdata,
                                (add->types->len > 1)
                                    ? g_ptr_array_index(add->types, 0)
                                    : NULL);

    /* the terminator is not a type; drop it now the combo has its copy */
    g_ptr_array_set_size(add->types, add->types->len - 1);

    add->summary = clawt_lrg_markdown(app, "");
    lrg_widget_add_class(add->summary, "muted");

    clawt_lrg_form_add(form, "Name", add->name,
                       "How you refer to it later; for an MCP server, also "
                       "its key in every agent's .mcp.json.");

    /*
     * "What it is", not "Kind": ClawtIntegrationKind is a real thing
     * here and means which direction an integration runs.
     */
    clawt_lrg_form_add(form, "What it is", add->type, NULL);
    clawt_lrg_take(form, add->summary);
    lrg_widget_set_min_size(form, 540.0f * (gfloat)app->scale, 0.0f);

    g_signal_connect(add->type, "changed", G_CALLBACK(on_add_type_changed),
                     add);
    on_add_type_changed(NULL, add);

    dialog = clawt_lrg_dialog(
        app, "Add an integration",
        "One connection, pointed at whichever agents should have it.",
        form, "Add", FALSE, on_add_response, add, add_release);
    g_set_weak_pointer(&add->dialog, dialog);
    g_object_unref(dialog);
    g_object_unref(form);
}

static void
on_add_clicked(LrgButton *button, gpointer user_data)
{
    clawt_lrg_request(user_data, "integration.types", NULL,
                      CLAWT_LRG_REQUEST_NONE, on_types, NULL, NULL);
}

/* ── The list ─────────────────────────────────────────────────────── */

/*
 * Two agents sharing one account, and the like: shown here rather than
 * only in the daemon's log, because it looks like the fleet
 * misbehaving rather than like a config mistake.
 */
static void
append_warning_rows(LrgWidget *list, JsonNode *reply)
{
    JsonArray *warnings = clawt_lrg_settings_member_array(
        clawt_ipc_reply_object(reply), "warnings");
    guint i;

    for (i = 0; warnings != NULL && i < json_array_get_length(warnings);
         i++) {
        LrgWidget *row = clawt_lrg_row(
            "Worth fixing", json_array_get_string_element(warnings, i));

        clawt_lrg_row_add_prefix(row, clawt_lrg_badge("!", "warn"));
        clawt_lrg_take(list, row);
    }
}

static void
on_list(ClawtLrgApp *app, JsonNode *reply, const GError *error,
        gpointer user_data)
{
    JsonArray *list;
    guint i;

    if (GPOINTER_TO_UINT(user_data) != integrations.generation ||
        reply == NULL)
        return;

    clawt_lrg_clear(integrations.list);
    list = clawt_lrg_settings_member_array(clawt_ipc_reply_object(reply),
                                           "integrations");

    if (list == NULL || json_array_get_length(list) == 0)
        clawt_lrg_take(integrations.list, clawt_lrg_row(
            "Nothing connected yet",
            "Add one to put a person on the other end of an agent's reply, "
            "or to give it a server's tools."));

    for (i = 0; list != NULL && i < json_array_get_length(list); i++) {
        JsonObject *integration = json_array_get_object_element(list, i);
        const gchar *name = clawt_ipc_payload_string(integration, "name");
        g_autofree gchar *subtitle =
            clawt_lrg_settings_integration_subtitle(integration);
        LrgWidget *row;
        LrgWidget *edit;

        if (name == NULL)
            continue;

        row = clawt_lrg_row(name, subtitle);
        edit = clawt_lrg_button("Edit…", "flat",
                                G_CALLBACK(on_edit_clicked), app);
        g_object_set_data_full(G_OBJECT(edit), "clawt-integration",
                               g_strdup(name), g_free);
        clawt_lrg_row_add_suffix(row, edit);
        clawt_lrg_row_add_prefix(row, clawt_lrg_badge(
            clawt_ipc_payload_boolean(integration, "enabled", TRUE) ? "on"
                                                                    : "off",
            clawt_ipc_payload_boolean(integration, "enabled", TRUE)
                ? "good" : "neutral"));
        clawt_lrg_appear(app, row, i);
        clawt_lrg_take(integrations.list, row);
    }

    append_warning_rows(integrations.list, reply);
    clawt_lrg_app_wake(app);
}

static void
refresh(ClawtLrgApp *app)
{
    if (integrations.list == NULL)
        return;

    integrations.generation++;
    clawt_lrg_request(app, "integration.list", NULL, CLAWT_LRG_QUIET,
                      on_list, GUINT_TO_POINTER(integrations.generation),
                      NULL);
}

static LrgWidget *
build(ClawtLrgApp *app)
{
    LrgWidget *page = clawt_lrg_page_new("Integrations",
                                         "Connections to the outside");
    LrgWidget *box = clawt_lrg_vbox(10.0f);

    clawt_lrg_take(clawt_lrg_page_toolbar(page),
                   clawt_lrg_button("Add an integration", "primary",
                                    G_CALLBACK(on_add_clicked), app));

    clawt_lrg_take(box, clawt_lrg_paragraph(
        "Configured once here and pointed at one agent, some agents or the "
        "whole fleet. A channel puts a person on the other end of an "
        "agent's reply; an MCP server gives it tools.", "muted"));

    integrations.list = clawt_lrg_vbox(6.0f);
    clawt_lrg_add(box, integrations.list);
    g_object_unref(integrations.list);

    clawt_lrg_take(clawt_lrg_page_body(page), clawt_lrg_card(box));

    return page;
}

static const ClawtLrgSettingsPage page_impl = {
    "integrations", "Integrations", build, refresh
};

const ClawtLrgSettingsPage *
clawt_lrg_settings_integrations_page(void)
{
    return &page_impl;
}
