/*
 * gtk-prefs-integrations.c - Settings: integrations
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The integrations that give an agent a channel, the editor for one,
 * and the Matrix sign-in and room picker that two of them need.
 *
 * One of the pages split out of clawt-window.c.  Everything it needs
 * from the window -- the instance struct, and the helpers more than one
 * page uses -- comes from clawt-window-private.h.
 */

#include "clawt-window-private.h"

/* ── Integrations ────────────────────────────────────────────────── */

/*
 * One integration being edited.
 *
 * The per-type rows are keyed by config key rather than held in named
 * members, because there used to be twenty of those and three places
 * that had to agree about them: the builder, the saver, and the Matrix
 * sign-in flow.  They did not -- the saver wrote `require_mention` and
 * the builder was the only thing that knew `folders` existed at all.
 *
 * What is *in* the table comes from clawt_integration_fields(), which
 * the web client draws from too, so the two cannot come to call a field
 * two different things.
 *
 * Values are unowned: the widgets belong to their preferences group.
 */
typedef struct {
    ClawtWindow *window;
    AdwDialog   *dialog;
    gchar       *name;
    gchar       *type_id;

    GtkWidget   *enabled_row;
    GtkWidget   *description_row;
    GtkWidget   *scope_row;
    GtkWidget   *agents_group;
    GPtrArray   *agent_rows;     /* AdwSwitchRow*, unowned */

    /*
     * key -> GtkWidget*.  A flags field's choices are keyed
     * "events.question" and so on, so one table holds every control.
     */
    GHashTable  *rows;

    GStrv        rooms;          /* what the picker last agreed on */
} IntegrationDialog;

/*
 * The control for one config key, or %NULL when this type has none.
 *
 * NULL rather than an assertion because a type's fields depend on the
 * type: asking a webhook for its homeserver is a question with a real
 * answer, and the answer is "there isn't one".
 */
static GtkWidget *
field_row(IntegrationDialog *dialog, const gchar *key)
{
    if (dialog->rows == NULL)
        return NULL;

    return g_hash_table_lookup(dialog->rows, key);
}

static void open_integration_editor(ClawtWindow *self, const gchar *name,
                                    const gchar *type_id);
static const gchar *current_when_value(IntegrationDialog *dialog,
                                      const gchar       *when_key);

static void
integration_dialog_free(gpointer data)
{
    IntegrationDialog *dialog = data;

    g_free(dialog->name);
    g_free(dialog->type_id);
    g_strfreev(dialog->rooms);
    g_clear_pointer(&dialog->agent_rows, g_ptr_array_unref);
    g_clear_pointer(&dialog->rows, g_hash_table_unref);
    g_free(dialog);
}

/*
 * The instance as the daemon currently has it.
 *
 * Refetched rather than cached on the dialog, because signing in changes
 * it behind the dialog's back: the daemon writes the user id and the
 * token reference itself, and a dialog showing what it had before would
 * save the old values back over them.
 */
JsonObject *
clawt_gtk_find_integration(JsonNode *reply, const gchar *name)
{
    JsonArray *integrations;
    guint i;

    if (reply == NULL)
        return NULL;

    integrations = json_object_get_array_member(json_node_get_object(reply),
                                                "integrations");

    for (i = 0; i < json_array_get_length(integrations); i++) {
        JsonObject *integration = json_array_get_object_element(integrations,
                                                                i);

        if (g_strcmp0(clawt_json_string(integration, "name", ""), name) == 0)
            return integration;
    }

    return NULL;
}

static gchar *
join_strings(JsonObject *object, const gchar *member, const gchar *separator)
{
    GString *out = g_string_new(NULL);
    JsonArray *array;
    guint i;

    if (object == NULL || !json_object_has_member(object, member))
        return g_string_free(out, FALSE);

    array = json_object_get_array_member(object, member);

    for (i = 0; i < json_array_get_length(array); i++) {
        if (i > 0)
            g_string_append(out, separator);

        g_string_append(out, json_array_get_string_element(array, i));
    }

    return g_string_free(out, FALSE);
}

/*
 * Adds a comma-separated entry as a JSON array.
 *
 * The separator is a comma because these are ids and room addresses,
 * which never contain one, and a person editing three rooms in a text
 * field should not have to think about quoting.
 */
static void
add_list_member(JsonBuilder *builder, const gchar *member, const gchar *text)
{
    g_auto(GStrv) parts = NULL;
    guint i;

    json_builder_set_member_name(builder, member);
    json_builder_begin_array(builder);

    if (text != NULL && *text != '\0') {
        parts = g_strsplit(text, ",", -1);

        for (i = 0; parts[i] != NULL; i++) {
            g_strstrip(parts[i]);

            if (*parts[i] != '\0')
                json_builder_add_string_value(builder, parts[i]);
        }
    }

    json_builder_end_array(builder);
}

static void
add_string_member(JsonBuilder *builder, const gchar *member, GtkWidget *row)
{
    if (row == NULL)
        return;

    json_builder_set_member_name(builder, member);
    json_builder_add_string_value(builder,
                                  gtk_editable_get_text(GTK_EDITABLE(row)));
}

static void
add_int_member(JsonBuilder *builder, const gchar *member, GtkWidget *row)
{
    const gchar *text;

    if (row == NULL)
        return;

    text = gtk_editable_get_text(GTK_EDITABLE(row));

    json_builder_set_member_name(builder, member);
    json_builder_add_int_value(builder, g_ascii_strtoll(text, NULL, 10));
}

static void
on_integration_saved(GtkButton *button, gpointer user_data)
{
    IntegrationDialog *dialog = user_data;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonNode) reply = NULL;
    guint i;

    (void)button;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, dialog->name);

    json_builder_set_member_name(builder, "enabled");
    json_builder_add_boolean_value(
        builder, adw_switch_row_get_active(ADW_SWITCH_ROW(dialog->enabled_row)));

    add_string_member(builder, "description", dialog->description_row);

    {
        static const gchar *const scopes[] = { "all", "selected", "none" };
        guint selected = adw_combo_row_get_selected(
            ADW_COMBO_ROW(dialog->scope_row));

        json_builder_set_member_name(builder, "scope");
        json_builder_add_string_value(builder, scopes[MIN(selected, 2)]);
    }

    /*
     * The ticked agents go up whatever the scope is, so switching to
     * `all` and back does not lose the selection somebody made.
     */
    json_builder_set_member_name(builder, "agents");
    json_builder_begin_array(builder);

    for (i = 0; i < dialog->agent_rows->len; i++) {
        GtkWidget *row = g_ptr_array_index(dialog->agent_rows, i);

        if (adw_switch_row_get_active(ADW_SWITCH_ROW(row)))
            json_builder_add_string_value(
                builder, g_object_get_data(G_OBJECT(row), "agent"));
    }

    json_builder_end_array(builder);

    /*
     * The same table the form was built from, and the same predicate for
     * what applies -- so a field that was not shown is not saved, and
     * the two cannot come to disagree about which those were.
     *
     * This used to be a second `if (type == ...)` chain beside the
     * builder's, and they had already drifted: the saver wrote
     * `require_mention` for matrix and knew nothing about `folders`,
     * which the builder had never offered either.
     */
    {
        const ClawtIntegrationField *list;
        gsize n = 0;
        gsize f;

        list = clawt_integration_fields(dialog->type_id, &n);

        for (f = 0; f < n; f++) {
            const ClawtIntegrationField *field = &list[f];
            const gchar *when = current_when_value(dialog, field->when_key);
            GtkWidget *row;

            if (!clawt_integration_field_applies(field, when))
                continue;

            if (field->kind == CLAWT_FIELD_FLAGS) {
                gsize c;

                json_builder_set_member_name(builder, field->key);
                json_builder_begin_array(builder);

                for (c = 0; field->choices[c] != NULL; c++) {
                    g_autofree gchar *key = g_strdup_printf(
                        "%s.%s", field->key, field->choices[c]);
                    GtkWidget *one = field_row(dialog, key);

                    if (one != NULL &&
                        adw_switch_row_get_active(ADW_SWITCH_ROW(one)))
                        json_builder_add_string_value(builder,
                                                      field->choices[c]);
                }

                json_builder_end_array(builder);
                continue;
            }

            row = field->when_key != NULL
                ? g_hash_table_lookup(dialog->rows, field->label)
                : field_row(dialog, field->key);

            if (row == NULL)
                continue;

            switch (field->kind) {
            case CLAWT_FIELD_BOOLEAN:
                json_builder_set_member_name(builder, field->key);
                json_builder_add_boolean_value(
                    builder, adw_switch_row_get_active(ADW_SWITCH_ROW(row)));
                break;

            case CLAWT_FIELD_CHOICE: {
                guint selected =
                    adw_combo_row_get_selected(ADW_COMBO_ROW(row));
                gsize c;
                const gchar *value = field->choices[0];

                for (c = 0; field->choices[c] != NULL; c++) {
                    if (c == selected)
                        value = field->choices[c];
                }

                json_builder_set_member_name(builder, field->key);
                json_builder_add_string_value(builder, value);
                break;
            }

            case CLAWT_FIELD_LIST:
                add_list_member(builder, field->key,
                                gtk_editable_get_text(GTK_EDITABLE(row)));
                break;

            case CLAWT_FIELD_INT:
                add_int_member(builder, field->key, row);
                break;

            case CLAWT_FIELD_SECRET: {
                /*
                 * A secret is sent as a reference -- env:NAME, file:PATH
                 * -- because there is no way to put a secret's value into
                 * clawtilla.yaml and this dialog is not going to be the
                 * first.  Empty means keep what is set: clearing one by
                 * accident costs an authorization nobody can see the
                 * reason for.
                 */
                const gchar *text =
                    gtk_editable_get_text(GTK_EDITABLE(row));
                g_auto(GStrv) parts = NULL;

                if (text == NULL || *text == '\0')
                    break;

                parts = g_strsplit(text, ":", 2);

                if (parts[1] == NULL || *parts[1] == '\0') {
                    clawt_window_toast(dialog->window,
                                       "A secret is a reference: env:NAME, "
                                       "file:PATH or command:... -- not the "
                                       "value itself.");
                    return;
                }

                json_builder_set_member_name(builder, "secret_key");
                json_builder_add_string_value(builder, field->key);
                json_builder_set_member_name(builder, "secret_backend");
                json_builder_add_string_value(builder, parts[0]);
                json_builder_set_member_name(builder, "secret_locator");
                json_builder_add_string_value(builder, parts[1]);
                break;
            }

            case CLAWT_FIELD_TEXT:
            default:
                add_string_member(builder, field->key, row);
                break;
            }
        }
    }

    json_builder_end_object(builder);

    reply = clawt_window_request(dialog->window, "integration.update",
                                 json_builder_get_root(builder));

    if (reply == NULL)
        return;

    clawt_gtk_refresh_settings_integrations(dialog->window);
    clawt_gtk_refresh_selected(dialog->window);
    adw_dialog_close(dialog->dialog);
}

static void
on_integration_removed(GtkButton *button, gpointer user_data)
{
    IntegrationDialog *dialog = user_data;
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(JsonBuilder) builder = json_builder_new();

    (void)button;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, dialog->name);
    json_builder_end_object(builder);

    reply = clawt_window_request(dialog->window, "integration.remove",
                                 json_builder_get_root(builder));

    if (reply == NULL)
        return;

    clawt_window_toast(dialog->window,
                       "Removed. Any credential file it wrote is still on "
                       "disk.");
    clawt_gtk_refresh_settings_integrations(dialog->window);
    clawt_gtk_refresh_selected(dialog->window);
    adw_dialog_close(dialog->dialog);
}

/*
 * Checks one integration against the first agent that has it.
 *
 * A health check belongs to a binding rather than to an instance -- an
 * instance shared by four agents may be reachable for one of them and
 * not another, if they differ in per_agent -- so it needs an agent to
 * check as, and the first in scope is the one that will notice first.
 */
static void
on_integration_checked(GtkButton *button, gpointer user_data)
{
    IntegrationDialog *dialog = user_data;
    g_autoptr(JsonNode) list = NULL;
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    JsonObject *integration;
    JsonArray *effective;
    JsonArray *checks;
    guint i;

    (void)button;

    /*
     * A notifier has nothing to connect to and check: the only honest
     * test is to send one and see whether it arrives.  It ignores the
     * event list and the quiet hours, because a button that did nothing
     * at half past eleven would be indistinguishable from a broken one.
     */
    if (g_strcmp0(dialog->type_id, "notify") == 0) {
        reply = clawt_window_request(
            dialog->window, "integration.notify_test",
            clawt_build_payload("integration", dialog->name, NULL));

        if (reply != NULL)
            clawt_window_toast(dialog->window,
                               "Sent. If nothing arrived, it is not "
                               "reaching you.");

        return;
    }

    list = clawt_window_request(dialog->window, "integration.list", NULL);
    integration = clawt_gtk_find_integration(list, dialog->name);

    if (integration == NULL)
        return;

    effective = json_object_get_array_member(integration,
                                             "effective_agents");

    if (json_array_get_length(effective) == 0) {
        clawt_window_toast(dialog->window,
                           "No agent has this yet, so there is nothing to "
                           "check it as.");
        return;
    }

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "agent");
    json_builder_add_string_value(builder,
                                  json_array_get_string_element(effective, 0));
    json_builder_set_member_name(builder, "integration");
    json_builder_add_string_value(builder, dialog->name);
    json_builder_end_object(builder);

    reply = clawt_window_request(dialog->window, "integration.health",
                                 json_builder_get_root(builder));

    if (reply == NULL)
        return;

    checks = json_object_get_array_member(json_node_get_object(reply),
                                          "checks");

    for (i = 0; i < json_array_get_length(checks); i++) {
        JsonObject *check = json_array_get_object_element(checks, i);

        if (json_object_get_boolean_member(check, "ok"))
            clawt_window_toast(dialog->window, "It answered.");
        else
            clawt_window_toast(dialog->window,
                               clawt_json_string(check, "error",
                                                 "it did not answer"));
    }
}

/* ── Matrix sign-in ──────────────────────────────────────────────── */

typedef struct {
    IntegrationDialog *editor;
    AdwDialog         *dialog;
    GtkWidget         *user_row;
    GtkWidget         *password_row;
} MatrixSignIn;

static void
on_matrix_signed_in(GtkButton *button, gpointer user_data)
{
    MatrixSignIn *sign_in = user_data;
    IntegrationDialog *editor = sign_in->editor;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonNode) reply = NULL;
    const gchar *password =
        gtk_editable_get_text(GTK_EDITABLE(sign_in->password_row));

    (void)button;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "integration");
    json_builder_add_string_value(builder, editor->name);
    json_builder_set_member_name(builder, "homeserver");
    json_builder_add_string_value(
        builder,
        gtk_editable_get_text(
            GTK_EDITABLE(field_row(editor, "homeserver"))));
    json_builder_set_member_name(builder, "user");
    json_builder_add_string_value(
        builder, gtk_editable_get_text(GTK_EDITABLE(sign_in->user_row)));
    json_builder_set_member_name(builder, "password");
    json_builder_add_string_value(builder, password);
    json_builder_end_object(builder);

    reply = clawt_window_request(editor->window, "integration.matrix_login",
                                 json_builder_get_root(builder));

    /*
     * Cleared whatever happened.  A wrong password leaves the dialog open
     * to be retyped, and leaving the old one in the box means the next
     * attempt sends it again by accident.
     */
    gtk_editable_set_text(GTK_EDITABLE(sign_in->password_row), "");

    if (reply == NULL)
        return;

    {
        JsonObject *root = json_node_get_object(reply);

        gtk_editable_set_text(GTK_EDITABLE(field_row(editor, "user_id")),
                              clawt_json_string(root, "user_id", ""));
    }

    clawt_window_toast(editor->window,
                       "Signed in. The token is on the daemon's disk, not "
                       "here.");
    adw_dialog_close(sign_in->dialog);
}

static void
on_matrix_sign_in(GtkButton *button, gpointer user_data)
{
    IntegrationDialog *editor = user_data;
    MatrixSignIn *sign_in = g_new0(MatrixSignIn, 1);
    AdwDialog *dialog = adw_dialog_new();
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *group = adw_preferences_group_new();
    GtkWidget *toolbar = adw_toolbar_view_new();
    GtkWidget *sign_in_button;

    (void)button;

    sign_in->editor = editor;
    sign_in->dialog = dialog;

    adw_dialog_set_title(dialog, "Sign in to Matrix");
    adw_dialog_set_content_width(dialog, 460);

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group),
                                    "Matrix account");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        "The password is used once, by the daemon, and is never stored. "
        "What comes back is an access token, written to a file only the "
        "daemon can read. It appears on your account's device list as "
        "\"clawtilla\", which is where you revoke it.");

    sign_in->user_row = adw_entry_row_new();
    adw_preferences_row_set_use_markup(
        ADW_PREFERENCES_ROW(sign_in->user_row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(sign_in->user_row),
                                  "User");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              sign_in->user_row);

    sign_in->password_row = adw_password_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(sign_in->password_row),
                                  "Password");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              sign_in->password_row);

    sign_in_button = gtk_button_new_with_label("Sign in");
    gtk_widget_add_css_class(sign_in_button, "suggested-action");
    gtk_widget_set_margin_top(sign_in_button, 12);
    g_signal_connect(sign_in_button, "clicked",
                     G_CALLBACK(on_matrix_signed_in), sign_in);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), sign_in_button);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(group));

    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar),
                                 adw_header_bar_new());
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), page);
    adw_dialog_set_child(dialog, toolbar);

    g_object_set_data_full(G_OBJECT(dialog), "sign-in", sign_in, g_free);
    adw_dialog_present(dialog, GTK_WIDGET(editor->window));
}

/* ── Choosing rooms ──────────────────────────────────────────────── */

static void
on_rooms_chosen(GtkButton *button, gpointer user_data)
{
    GPtrArray *rows = g_object_get_data(G_OBJECT(button), "rows");
    IntegrationDialog *editor = user_data;
    GString *chosen = g_string_new(NULL);
    g_autofree gchar *text = NULL;
    guint i;

    for (i = 0; i < rows->len; i++) {
        GtkWidget *row = g_ptr_array_index(rows, i);

        if (!adw_switch_row_get_active(ADW_SWITCH_ROW(row)))
            continue;

        if (chosen->len > 0)
            g_string_append(chosen, ", ");

        g_string_append(chosen, g_object_get_data(G_OBJECT(row), "room"));
    }

    text = g_string_free(chosen, FALSE);
    gtk_editable_set_text(GTK_EDITABLE(field_row(editor, "rooms")), text);

    adw_dialog_close(ADW_DIALOG(g_object_get_data(G_OBJECT(button),
                                                  "dialog")));
}

static void
on_choose_rooms(GtkButton *button, gpointer user_data)
{
    IntegrationDialog *editor = user_data;
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_auto(GStrv) current = NULL;
    AdwDialog *dialog;
    GtkWidget *page;
    GtkWidget *group;
    GtkWidget *toolbar;
    GtkWidget *done;
    GPtrArray *rows;
    JsonArray *rooms;
    guint i;

    (void)button;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "integration");
    json_builder_add_string_value(builder, editor->name);
    json_builder_end_object(builder);

    reply = clawt_window_request(editor->window, "integration.matrix_rooms",
                                 json_builder_get_root(builder));

    if (reply == NULL)
        return;

    rooms = json_object_get_array_member(json_node_get_object(reply),
                                         "rooms");

    dialog = adw_dialog_new();
    adw_dialog_set_title(dialog, "Rooms");
    adw_dialog_set_content_width(dialog, 520);
    adw_dialog_set_content_height(dialog, 560);

    page = adw_preferences_page_new();
    group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group),
                                    "Listen in");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        "Choose none to listen in every room this account is in, including "
        "ones it is invited to later.");

    current = g_strsplit(
        gtk_editable_get_text(GTK_EDITABLE(field_row(editor, "rooms"))),
        ",", -1);
    rows = g_ptr_array_new();

    for (i = 0; i < json_array_get_length(rooms); i++) {
        JsonObject *room = json_array_get_object_element(rooms, i);
        const gchar *id = clawt_json_string(room, "id", "");
        GtkWidget *row = adw_switch_row_new();
        guint k;

        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                      clawt_json_string(room, "label", id));
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), id);

        for (k = 0; current[k] != NULL; k++) {
            if (g_strcmp0(g_strstrip(current[k]), id) == 0)
                adw_switch_row_set_active(ADW_SWITCH_ROW(row), TRUE);
        }

        g_object_set_data_full(G_OBJECT(row), "room", g_strdup(id), g_free);
        g_ptr_array_add(rows, row);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
    }

    if (json_array_get_length(rooms) == 0)
        adw_preferences_group_set_description(
            ADW_PREFERENCES_GROUP(group),
            "That account is not in any rooms yet. Invite it from your "
            "Matrix client and look again.");

    done = gtk_button_new_with_label("Use these");
    gtk_widget_add_css_class(done, "suggested-action");
    gtk_widget_set_margin_top(done, 12);
    g_object_set_data_full(G_OBJECT(done), "rows", rows,
                           (GDestroyNotify)g_ptr_array_unref);
    g_object_set_data(G_OBJECT(done), "dialog", dialog);
    g_signal_connect(done, "clicked", G_CALLBACK(on_rooms_chosen), editor);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), done);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(group));

    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar),
                                 adw_header_bar_new());
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), page);
    adw_dialog_set_child(dialog, toolbar);

    adw_dialog_present(dialog, GTK_WIDGET(editor->window));
}

/* ── The editor ──────────────────────────────────────────────────── */

GtkWidget *
clawt_gtk_add_entry(GtkWidget *group, const gchar *title, const gchar *value)
{
    GtkWidget *row = adw_entry_row_new();

    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);

    if (value != NULL)
        gtk_editable_set_text(GTK_EDITABLE(row), value);

    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);

    return row;
}

/*
 * The agent list, one switch each.
 *
 * Switches rather than a multi-select list because the question is per
 * agent -- "does the researcher get this" -- and a selection model makes
 * that a drag gesture with a hidden state instead of a yes or a no.
 */
static void
build_agent_group(IntegrationDialog *dialog, JsonObject *integration)
{
    g_autoptr(JsonNode) agents = NULL;
    g_autofree gchar *chosen = NULL;
    JsonArray *array;
    guint i;

    dialog->agents_group = adw_preferences_group_new();
    adw_preferences_group_set_title(
        ADW_PREFERENCES_GROUP(dialog->agents_group), "Which agents");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(dialog->agents_group),
        "Used when the scope above is \"These agents\". Kept either way, so "
        "switching to everyone and back does not lose the choice.");

    dialog->agent_rows = g_ptr_array_new();
    chosen = join_strings(integration, "agents", ",");
    agents = clawt_window_request(dialog->window, "agent.list", NULL);

    if (agents == NULL)
        return;

    array = json_object_get_array_member(json_node_get_object(agents),
                                         "agents");

    for (i = 0; i < json_array_get_length(array); i++) {
        JsonObject *agent = json_array_get_object_element(array, i);
        const gchar *id = clawt_json_string(agent, "id", "");
        GtkWidget *row = adw_switch_row_new();
        g_auto(GStrv) parts = g_strsplit(chosen, ",", -1);
        guint k;

        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                      clawt_json_string(agent, "name", id));
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), id);

        for (k = 0; parts[k] != NULL; k++) {
            if (g_strcmp0(g_strstrip(parts[k]), id) == 0)
                adw_switch_row_set_active(ADW_SWITCH_ROW(row), TRUE);
        }

        g_object_set_data_full(G_OBJECT(row), "agent", g_strdup(id), g_free);
        g_ptr_array_add(dialog->agent_rows, row);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(dialog->agents_group),
                                  row);
    }
}

/*
 * Whichever value decides what else is shown, resolved.
 *
 * An instance that has never had a backend written is on the field's
 * default, and passing NULL through would hide every field of the
 * backend it is actually using -- which is the old bug facing the other
 * way.
 */
static const gchar *
current_when_value(IntegrationDialog *dialog, const gchar *when_key)
{
    const ClawtIntegrationField *list;
    gsize n = 0;
    gsize i;
    GtkWidget *row;

    if (when_key == NULL)
        return NULL;

    list = clawt_integration_fields(dialog->type_id, &n);

    for (i = 0; i < n; i++) {
        if (g_strcmp0(list[i].key, when_key) != 0)
            continue;

        /*
         * By label when the deciding field is itself conditional, since
         * that is how build_type_rows() keyed it.  Nothing is today, and
         * a lookup that silently returned NULL would fall back to the
         * default and hide every dependent field.
         */
        row = list[i].when_key != NULL
            ? g_hash_table_lookup(dialog->rows, list[i].label)
            : field_row(dialog, when_key);

        if (list[i].kind == CLAWT_FIELD_CHOICE && row != NULL) {
            guint selected =
                adw_combo_row_get_selected(ADW_COMBO_ROW(row));
            gsize c;

            for (c = 0; list[i].choices[c] != NULL; c++) {
                if (c == selected)
                    return list[i].choices[c];
            }
        }

        return clawt_integration_field_default(&list[i]);
    }

    return NULL;
}

/*
 * Hides the rows that do not apply to what is currently chosen.
 *
 * Hidden rather than destroyed and rebuilt: AdwPreferencesGroup cannot
 * enumerate its own rows, so a rebuild means keeping a second list of
 * them and is a second place to forget one.  A hidden AdwPreferencesRow
 * is a hidden GtkListBoxRow, which the list simply skips.
 *
 * The save reads the same predicate rather than the widget's visibility,
 * so the two cannot disagree about whether a field counted.
 */
static void
apply_field_visibility(IntegrationDialog *dialog)
{
    const ClawtIntegrationField *list;
    gsize n = 0;
    gsize i;

    list = clawt_integration_fields(dialog->type_id, &n);

    for (i = 0; i < n; i++) {
        const gchar *when = current_when_value(dialog, list[i].when_key);
        gboolean applies = clawt_integration_field_applies(&list[i], when);
        GtkWidget *row;

        /*
         * A flags field is several rows, one per choice, so it is hidden
         * by hiding each of them.  No flags field has a condition today
         * and this costs nothing until one does -- the alternative is a
         * branch that silently does not run, which is how a rule ends up
         * threaded through and never read.
         */
        if (list[i].kind == CLAWT_FIELD_FLAGS) {
            gsize c;

            for (c = 0; list[i].choices != NULL &&
                        list[i].choices[c] != NULL; c++) {
                g_autofree gchar *key = g_strdup_printf(
                    "%s.%s", list[i].key, list[i].choices[c]);
                GtkWidget *one = field_row(dialog, key);

                if (one != NULL)
                    gtk_widget_set_visible(one, applies);
            }

            continue;
        }

        /*
         * Two fields can share a key -- notify's `url` is an ntfy topic
         * and a Gotify server -- so those are keyed by label, which is
         * what made them two entries in the first place.
         */
        row = list[i].when_key != NULL
            ? g_hash_table_lookup(dialog->rows, list[i].label)
            : field_row(dialog, list[i].key);

        if (row != NULL)
            gtk_widget_set_visible(row, applies);
    }
}

static void
on_choice_changed(GObject *object, GParamSpec *spec, gpointer user_data)
{
    (void)object;
    (void)spec;

    apply_field_visibility(user_data);
}

/*
 * Builds the form for one type, from the table both clients share.
 *
 * This used to be a chain of `if (type == "matrix") ... else if
 * (type == "email")` with a named widget per field, and a matching chain
 * in the saver.  The notify half built every backend's fields at once,
 * so choosing "Desktop notification" still asked for a Matrix homeserver,
 * a room, an ntfy URL, a token and a command line.
 */
static void
build_type_rows(IntegrationDialog *dialog, GtkWidget *group,
                JsonObject *integration)
{
    const ClawtIntegrationField *list;
    gsize n = 0;
    gsize i;

    list = clawt_integration_fields(dialog->type_id, &n);

    for (i = 0; i < n; i++) {
        const ClawtIntegrationField *field = &list[i];
        GtkWidget *row = NULL;

        switch (field->kind) {
        case CLAWT_FIELD_BOOLEAN:
            row = adw_switch_row_new();
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                          field->label);
            adw_switch_row_set_active(
                ADW_SWITCH_ROW(row),
                integration == NULL ||
                !json_object_has_member(integration, field->key) ||
                json_object_get_boolean_member(integration, field->key));
            adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
            break;

        case CLAWT_FIELD_CHOICE: {
            g_autoptr(GtkStringList) labels = gtk_string_list_new(NULL);
            const gchar *chosen = clawt_json_string(
                integration, field->key,
                clawt_integration_field_default(field));
            gsize c;

            row = adw_combo_row_new();
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                          field->label);

            for (c = 0; field->choices[c] != NULL; c++)
                gtk_string_list_append(labels, field->choice_labels[c]);

            /*
             * The model before the selection, which is not a style
             * preference: a position set on a combo with no items has
             * nothing to land on, and every one of these would have
             * opened showing its first choice -- then saved that back
             * over whatever was configured.
             */
            adw_combo_row_set_model(ADW_COMBO_ROW(row),
                                    G_LIST_MODEL(g_steal_pointer(&labels)));

            for (c = 0; field->choices[c] != NULL; c++) {
                if (g_strcmp0(field->choices[c], chosen) == 0)
                    adw_combo_row_set_selected(ADW_COMBO_ROW(row),
                                               (guint)c);
            }

            adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);

            /*
             * Connected after the selection is in place, so building the
             * form does not count as somebody changing it.
             */
            g_signal_connect(row, "notify::selected",
                             G_CALLBACK(on_choice_changed), dialog);
            break;
        }

        case CLAWT_FIELD_FLAGS: {
            g_autofree gchar *chosen =
                join_strings(integration, field->key, ",");
            gsize c;

            for (c = 0; field->choices[c] != NULL; c++) {
                GtkWidget *one = adw_switch_row_new();
                g_autofree gchar *key = g_strdup_printf(
                    "%s.%s", field->key, field->choices[c]);
                g_auto(GStrv) parts = g_strsplit(chosen, ",", -1);
                guint k;

                adw_preferences_row_set_title(ADW_PREFERENCES_ROW(one),
                                              field->choice_labels[c]);

                /*
                 * An instance with no list of its own is on the schema
                 * default, which is question and error -- so an empty
                 * list would show switches off that the daemon has on.
                 */
                if (*chosen == '\0') {
                    adw_switch_row_set_active(
                        ADW_SWITCH_ROW(one),
                        g_strcmp0(field->choices[c], "question") == 0 ||
                        g_strcmp0(field->choices[c], "error") == 0);
                } else {
                    for (k = 0; parts[k] != NULL; k++) {
                        if (g_strcmp0(g_strstrip(parts[k]),
                                      field->choices[c]) == 0)
                            adw_switch_row_set_active(ADW_SWITCH_ROW(one),
                                                      TRUE);
                    }
                }

                adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), one);
                g_hash_table_insert(dialog->rows, g_steal_pointer(&key), one);
            }

            continue;
        }

        case CLAWT_FIELD_LIST: {
            g_autofree gchar *joined =
                join_strings(integration, field->key, ", ");

            row = clawt_gtk_add_entry(group, field->label, joined);
            break;
        }

        case CLAWT_FIELD_SECRET:
            /*
             * Never the value.  There is no path that puts a secret into
             * clawtilla.yaml, so a box showing one would be showing a
             * reference at best and inviting a live token at worst.
             */
            row = clawt_gtk_add_entry(group, field->label, "");
            break;

        case CLAWT_FIELD_INT: {
            /*
             * Formatted rather than read as a string.  A port is a JSON
             * int on the wire, and the string reader answers a JSON int
             * with its fallback -- so every port would have opened this
             * dialog empty and been saved back as zero.
             */
            g_autofree gchar *text = NULL;

            if (integration != NULL &&
                json_object_has_member(integration, field->key))
                text = g_strdup_printf(
                    "%" G_GINT64_FORMAT,
                    json_object_get_int_member(integration, field->key));

            row = clawt_gtk_add_entry(group, field->label, text);
            break;
        }

        case CLAWT_FIELD_TEXT:
        default:
            row = clawt_gtk_add_entry(
                group, field->label,
                clawt_json_string(integration, field->key, ""));
            break;
        }

        if (row == NULL)
            continue;

        if (field->hint != NULL)
            clawt_gtk_set_row_hint(row, field->hint);

        /*
         * Keyed by label when two fields share a config key, so both
         * rows exist and only the applicable one is shown -- and by key
         * otherwise, which is what the Matrix flows and the saver look
         * things up by.
         */
        g_hash_table_insert(dialog->rows,
                            g_strdup(field->when_key != NULL ? field->label
                                                             : field->key),
                            row);

        if (field->when_key != NULL)
            continue;

        /* The two Matrix conveniences hang off their own rows. */
        if (g_strcmp0(dialog->type_id, "matrix") == 0 &&
            g_strcmp0(field->key, "homeserver") == 0) {
            GtkWidget *sign_in =
                gtk_button_new_with_label("Sign in\342\200\246");

            gtk_widget_set_valign(sign_in, GTK_ALIGN_CENTER);
            g_signal_connect(sign_in, "clicked",
                             G_CALLBACK(on_matrix_sign_in), dialog);
            adw_entry_row_add_suffix(ADW_ENTRY_ROW(row), sign_in);
        }

        if (g_strcmp0(dialog->type_id, "matrix") == 0 &&
            g_strcmp0(field->key, "rooms") == 0) {
            GtkWidget *choose =
                gtk_button_new_with_label("Choose\342\200\246");

            gtk_widget_set_valign(choose, GTK_ALIGN_CENTER);
            g_signal_connect(choose, "clicked", G_CALLBACK(on_choose_rooms),
                             dialog);
            adw_entry_row_add_suffix(ADW_ENTRY_ROW(row), choose);
        }
    }

    apply_field_visibility(dialog);
}

static void
open_integration_editor(ClawtWindow *self, const gchar *name,
                        const gchar *type_id)
{
    IntegrationDialog *dialog = g_new0(IntegrationDialog, 1);
    g_autoptr(JsonNode) list = NULL;
    AdwDialog *window = adw_dialog_new();
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *group = adw_preferences_group_new();
    GtkWidget *actions = adw_preferences_group_new();
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *toolbar = adw_toolbar_view_new();
    GtkWidget *save;
    GtkWidget *check;
    GtkWidget *remove;
    JsonObject *integration;
    static const gchar *const scopes[] = {
        "Every agent", "These agents", "Nobody", NULL
    };

    list = clawt_window_request(self, "integration.list", NULL);
    integration = clawt_gtk_find_integration(list, name);

    dialog->window = self;
    dialog->dialog = window;
    dialog->rows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                         NULL);
    dialog->name = g_strdup(name);
    dialog->type_id = g_strdup(
        integration != NULL ? clawt_json_string(integration, "type", type_id)
                            : type_id);

    adw_dialog_set_title(window, name);
    adw_dialog_set_content_width(window, 560);
    adw_dialog_set_content_height(window, 680);

    adw_preferences_group_set_title(
        ADW_PREFERENCES_GROUP(group),
        clawt_integration_type_label(dialog->type_id));
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        integration != NULL ? clawt_json_string(integration, "summary", "")
                            : "");

    dialog->enabled_row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->enabled_row),
                                  "Enabled");
    adw_switch_row_set_active(
        ADW_SWITCH_ROW(dialog->enabled_row),
        integration == NULL ||
        json_object_get_boolean_member(integration, "enabled"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              dialog->enabled_row);

    dialog->description_row = clawt_gtk_add_entry(
        group, "What it is for",
        clawt_json_string(integration, "description", ""));
    clawt_gtk_set_row_hint(dialog->description_row,
                           "Written into every agent's TOOLS.org");

    dialog->scope_row = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->scope_row),
                                  "Who gets it");
    adw_combo_row_set_model(ADW_COMBO_ROW(dialog->scope_row),
                            G_LIST_MODEL(gtk_string_list_new(scopes)));

    {
        const gchar *scope = clawt_json_string(integration, "scope",
                                               "selected");

        adw_combo_row_set_selected(
            ADW_COMBO_ROW(dialog->scope_row),
            g_strcmp0(scope, "all") == 0 ? 0
                : (g_strcmp0(scope, "none") == 0 ? 2 : 1));
    }

    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), dialog->scope_row);

    build_type_rows(dialog, group, integration);

    build_agent_group(dialog, integration);

    save = gtk_button_new_with_label("Save");
    gtk_widget_add_css_class(save, "suggested-action");
    gtk_widget_set_hexpand(save, TRUE);
    g_signal_connect(save, "clicked", G_CALLBACK(on_integration_saved),
                     dialog);

    check = gtk_button_new_with_label(
        g_strcmp0(dialog->type_id, "notify") == 0 ? "Send a test" : "Check");
    gtk_widget_set_hexpand(check, TRUE);
    g_signal_connect(check, "clicked", G_CALLBACK(on_integration_checked),
                     dialog);

    remove = gtk_button_new_with_label("Remove");
    gtk_widget_add_css_class(remove, "destructive-action");
    gtk_widget_set_hexpand(remove, TRUE);
    g_signal_connect(remove, "clicked", G_CALLBACK(on_integration_removed),
                     dialog);

    gtk_box_append(GTK_BOX(buttons), save);
    gtk_box_append(GTK_BOX(buttons), check);
    gtk_box_append(GTK_BOX(buttons), remove);
    gtk_widget_set_margin_top(buttons, 12);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(actions), buttons);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(group));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(dialog->agents_group));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(actions));

    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar),
                                 adw_header_bar_new());
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), page);
    adw_dialog_set_child(window, toolbar);

    g_object_set_data_full(G_OBJECT(window), "dialog", dialog,
                           integration_dialog_free);
    adw_dialog_present(window, GTK_WIDGET(self));
}

/* ── Adding one ──────────────────────────────────────────────────── */

typedef struct {
    ClawtWindow *window;
    AdwDialog   *dialog;
    GtkWidget   *name_row;
    GtkWidget   *type_row;
    GtkWidget   *summary_label;
    GStrv        types;
    GStrv        summaries;      /* parallel to `types` */
} AddIntegration;

/*
 * Says what the chosen type is for, and what it will need.
 *
 * Neither client said anything at all here, so picking one was a guess
 * and finding out what it wanted meant creating it first and reading the
 * form.
 */
static void
on_add_type_changed(GObject *object, GParamSpec *spec, gpointer user_data)
{
    AddIntegration *add = user_data;
    guint selected = adw_combo_row_get_selected(ADW_COMBO_ROW(object));

    (void)spec;

    if (add->summaries == NULL ||
        selected >= g_strv_length(add->summaries))
        return;

    gtk_label_set_text(GTK_LABEL(add->summary_label),
                       add->summaries[selected]);
}

static void
add_integration_free(gpointer data)
{
    AddIntegration *add = data;

    g_strfreev(add->types);
    g_strfreev(add->summaries);
    g_free(add);
}

static void
on_integration_added(GtkButton *button, gpointer user_data)
{
    AddIntegration *add = user_data;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonNode) reply = NULL;
    const gchar *name = gtk_editable_get_text(GTK_EDITABLE(add->name_row));
    guint selected = adw_combo_row_get_selected(ADW_COMBO_ROW(add->type_row));
    const gchar *type_id;

    (void)button;

    if (name == NULL || *name == '\0') {
        clawt_window_toast(add->window, "It needs a name.");
        return;
    }

    if (add->types == NULL || selected >= g_strv_length(add->types))
        return;

    type_id = add->types[selected];

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, name);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, type_id);
    json_builder_end_object(builder);

    reply = clawt_window_request(add->window, "integration.add",
                                 json_builder_get_root(builder));

    if (reply == NULL)
        return;

    clawt_gtk_refresh_settings_integrations(add->window);
    adw_dialog_close(add->dialog);

    /*
     * Straight into the editor.  An integration with nothing but a name
     * and a type reaches nobody and does nothing, so stopping here would
     * be stopping halfway.
     */
    open_integration_editor(add->window, name, type_id);
}

static void
on_add_integration(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    AddIntegration *add = g_new0(AddIntegration, 1);
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(GPtrArray) ids = NULL;
    AdwDialog *dialog = adw_dialog_new();
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *group = adw_preferences_group_new();
    GtkWidget *toolbar = adw_toolbar_view_new();
    GtkStringList *labels = gtk_string_list_new(NULL);
    g_autoptr(GPtrArray) summaries = NULL;
    GtkWidget *create;
    JsonArray *types;
    guint i;

    (void)button;

    add->window = self;
    add->dialog = dialog;

    reply = clawt_window_request(self, "integration.types", NULL);

    if (reply == NULL)
        return;

    types = json_object_get_array_member(json_node_get_object(reply),
                                         "types");
    ids = g_ptr_array_new();
    summaries = g_ptr_array_new_with_free_func(g_free);

    for (i = 0; i < json_array_get_length(types); i++) {
        JsonObject *type = json_array_get_object_element(types, i);
        const gchar *id = clawt_json_string(type, "id", "");

        /*
         * The name a person would say, not the config value.  This read
         * "mcp -- Give agents the tools of any MCP server" in a combo
         * that shows one line at a time, so the summary was invisible
         * until you opened the list and the visible part was a lowercase
         * identifier.
         */
        gtk_string_list_append(labels, clawt_integration_type_label(id));
        g_ptr_array_add(ids, g_strdup(id));

        {
            g_autofree gchar *needs = clawt_integration_needs_summary(id);

            g_ptr_array_add(summaries, g_strdup_printf(
                "%s%s%s", clawt_json_string(type, "summary", ""),
                needs != NULL ? "\n\n" : "",
                needs != NULL ? needs : ""));
        }
    }

    g_ptr_array_add(ids, NULL);
    g_ptr_array_add(summaries, NULL);
    add->types = (GStrv)g_ptr_array_free(g_steal_pointer(&ids), FALSE);

    adw_dialog_set_title(dialog, "Add an integration");
    adw_dialog_set_content_width(dialog, 560);

    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        "One connection, pointed at whichever agents should have it. The "
        "name is how you refer to it later, and for an MCP server it is "
        "also the key it gets in every agent's .mcp.json.");

    add->name_row = adw_entry_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(add->name_row),
                                       FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(add->name_row), "Name");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), add->name_row);

    /*
     * "What it is", not "Kind".  ClawtIntegrationKind is a real thing in
     * this codebase and means something else -- which direction an
     * integration runs in -- so a picker of types labelled Kind was
     * naming the wrong concept.
     */
    add->type_row = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(add->type_row),
                                  "What it is");
    adw_combo_row_set_model(ADW_COMBO_ROW(add->type_row),
                            G_LIST_MODEL(labels));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), add->type_row);

    /*
     * What the chosen one does, and what it will ask for.
     *
     * Under the picker rather than inside it, because a combo shows one
     * line and this is two sentences -- and because the question
     * somebody is actually asking here is "which of these do I want",
     * which needs the description of the one they are looking at.
     */
    add->summary_label = gtk_label_new(NULL);
    gtk_label_set_wrap(GTK_LABEL(add->summary_label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(add->summary_label), 0.0f);
    gtk_widget_add_css_class(add->summary_label, "dim-label");
    gtk_widget_set_margin_top(add->summary_label, 6);
    gtk_widget_set_margin_start(add->summary_label, 6);
    gtk_widget_set_margin_end(add->summary_label, 6);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              add->summary_label);

    add->summaries = g_strdupv((GStrv)summaries->pdata);
    g_signal_connect(add->type_row, "notify::selected",
                     G_CALLBACK(on_add_type_changed), add);
    on_add_type_changed(G_OBJECT(add->type_row), NULL, add);

    create = gtk_button_new_with_label("Add");
    gtk_widget_add_css_class(create, "suggested-action");
    gtk_widget_set_margin_top(create, 12);
    g_signal_connect(create, "clicked", G_CALLBACK(on_integration_added),
                     add);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), create);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(group));

    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar),
                                 adw_header_bar_new());
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), page);
    adw_dialog_set_child(dialog, toolbar);

    g_object_set_data_full(G_OBJECT(dialog), "add", add,
                           add_integration_free);
    adw_dialog_present(dialog, GTK_WIDGET(self));
}

/* ── The settings list ───────────────────────────────────────────── */

static void
on_integration_activated(GtkListBox *box, GtkListBoxRow *row,
                         gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *name;

    (void)box;

    if (row == NULL)
        return;

    name = g_object_get_data(G_OBJECT(row), "integration");

    if (name == NULL)
        return;

    open_integration_editor(self, name, NULL);
}

void
clawt_gtk_refresh_settings_integrations(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *integrations;
    JsonArray *warnings;
    guint i;

    if (self->settings_integrations == NULL)
        return;

    if (!clawt_gtk_refresh_enter(self, CLAWT_REFRESH_INTEGRATIONS))
        return;

    do {
        clawt_gtk_clear_list(GTK_LIST_BOX(self->settings_integrations));

        reply = clawt_window_request(self, "integration.list", NULL);

        if (reply == NULL)
            continue;

        integrations = json_object_get_array_member(
            json_node_get_object(reply), "integrations");

        for (i = 0; i < json_array_get_length(integrations); i++) {
            JsonObject *integration =
                json_array_get_object_element(integrations, i);
            const gchar *name = clawt_json_string(integration, "name", "?");
            JsonArray *effective =
                json_object_get_array_member(integration, "effective_agents");
            const gchar *scope = clawt_json_string(integration, "scope",
                                                   "selected");
            GtkWidget *row = adw_action_row_new();
            g_autofree gchar *subtitle = NULL;
            g_autofree gchar *reach = NULL;

            if (g_strcmp0(scope, "all") == 0)
                reach = g_strdup_printf("every agent (%u)",
                                        json_array_get_length(effective));
            else if (json_array_get_length(effective) == 0)
                reach = g_strdup("nobody yet");
            else
                reach = join_strings(integration, "effective_agents", ", ");

            subtitle = g_strdup_printf(
                "%s \342\200\224 %s%s",
                clawt_json_string(integration, "type", "?"), reach,
                json_object_get_boolean_member(integration, "enabled")
                    ? "" : " (off)");

            adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row),
                                               FALSE);
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), name);
            adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
            clawt_gtk_row_opens_something(row);

            g_object_set_data_full(G_OBJECT(row), "integration",
                                   g_strdup(name), g_free);
            gtk_list_box_append(GTK_LIST_BOX(self->settings_integrations),
                                row);
        }

        /*
         * A collision between two agents sharing one account is worth
         * showing here rather than only in the daemon's log: it is the
         * failure that looks like the fleet misbehaving rather than like
         * a config mistake.
         */
        warnings = json_object_get_array_member(json_node_get_object(reply),
                                                "warnings");

        for (i = 0; i < json_array_get_length(warnings); i++) {
            GtkWidget *row = adw_action_row_new();

            adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row),
                                               FALSE);
            adw_preferences_row_set_title(
                ADW_PREFERENCES_ROW(row),
                json_array_get_string_element(warnings, i));
            adw_action_row_add_prefix(
                ADW_ACTION_ROW(row),
                gtk_image_new_from_icon_name("dialog-warning-symbolic"));
            gtk_widget_set_sensitive(row, FALSE);
            gtk_list_box_append(GTK_LIST_BOX(self->settings_integrations),
                                row);
        }
    } while (clawt_gtk_refresh_repeat(self, CLAWT_REFRESH_INTEGRATIONS));
}

GtkWidget *
clawt_gtk_build_integrations_page(ClawtWindow *self)
{
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *group = adw_preferences_group_new();
    GtkWidget *add;

    adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(page),
                                   "Integrations");
    adw_preferences_page_set_icon_name(ADW_PREFERENCES_PAGE(page),
                                       "network-transmit-receive-symbolic");

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group),
                                    "Connections to the outside");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        "Configured once here and pointed at one agent, some agents or the "
        "whole fleet. A channel puts a person on the other end of an "
        "agent's reply; an MCP server gives it tools.");

    add = gtk_button_new_from_icon_name("list-add-symbolic");
    gtk_widget_set_tooltip_text(add, "Add an integration");
    gtk_widget_add_css_class(add, "flat");
    g_signal_connect(add, "clicked", G_CALLBACK(on_add_integration), self);
    adw_preferences_group_set_header_suffix(ADW_PREFERENCES_GROUP(group),
                                            add);

    /*
     * A list box of the group's own, for the same reason the image list
     * has one: refreshing means emptying one container rather than
     * working out which of a preferences group's children were ours.
     */
    self->settings_integrations = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(self->settings_integrations),
                                    GTK_SELECTION_NONE);
    gtk_widget_add_css_class(self->settings_integrations, "boxed-list");
    g_signal_connect(self->settings_integrations, "row-activated",
                     G_CALLBACK(on_integration_activated), self);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              self->settings_integrations);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(group));

    return page;
}
