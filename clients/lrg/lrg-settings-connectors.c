/*
 * lrg-settings-connectors.c - Settings: Connectors
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The accounts clawtilla holds a credential for, and the flows that
 * authorise one.  Mirrors clients/gtk/gtk-prefs-connectors.c: the list
 * with each connector's state (through clawt_connector_state(), the
 * rule both other clients use), adding one from the catalogue, the
 * device-code and browser flows, pasting a token, renewing, revoking,
 * removing, and importing the open MCP registry.
 *
 * `connector.await` waits on a person -- somebody unlocking a phone to
 * type a code -- so GTK gives it fifteen minutes on a blocking request.
 * Here it is an ordinary async request, which has no client-side
 * timeout at all: the reply arrives when the daemon has an answer, and
 * the window keeps drawing meanwhile.
 */

#include "clawt-lrg.h"
#include "clawt-lrg-settings-model.h"

#include <string.h>

typedef struct {
    LrgWidget *list;
    guint      generation;

    /* connectors.directory_min_tools and _min_chars, filled on refresh. */
    LrgWidget *min_tools;
    LrgWidget *min_chars;
} Connectors;

static Connectors connectors;

static void refresh(ClawtLrgApp *app);

/* ── Authorising ──────────────────────────────────────────────────── */

typedef struct {
    gchar     *name;
    LrgDialog *dialog;    /* weak: the code, while it waits */
} Flow;

static void
flow_free(gpointer data)
{
    Flow *flow = data;

    g_clear_weak_pointer(&flow->dialog);
    g_free(flow->name);
    g_free(flow);
}

static void
on_awaited(ClawtLrgApp *app, JsonNode *reply, const GError *error,
           gpointer user_data)
{
    Flow *flow = user_data;

    if (flow->dialog != NULL)
        lrg_dialog_close(flow->dialog);

    if (reply != NULL)
        clawt_lrg_toast(app, "Connected.");

    refresh(app);
}

static gboolean
on_code_copied(ClawtLrgApp *app, LrgDialog *dialog, gint response,
               gpointer user_data)
{
    clawt_lrg_copy(app, user_data);
    clawt_lrg_toast(app, "Copied.");

    /* still waiting: the code is about to be typed somewhere else */
    return FALSE;
}

static void
on_open_page(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = g_object_get_data(G_OBJECT(button), "clawt-app");

    clawt_lrg_open_uri(app, user_data);
}

static void
on_begun(ClawtLrgApp *app, JsonNode *reply, const GError *error,
         gpointer user_data)
{
    Flow *flow = user_data;
    JsonObject *root = clawt_ipc_reply_object(reply);
    const gchar *method = clawt_ipc_payload_string(root, "method");
    const gchar *flow_id = clawt_ipc_payload_string(root, "flow");
    LrgWidget *content = clawt_lrg_vbox(12.0f);
    Flow *waiting;
    LrgDialog *dialog;

    if (root == NULL || flow_id == NULL) {
        g_object_unref(content);
        return;
    }

    if (g_strcmp0(method, "device") == 0) {
        const gchar *uri = clawt_ipc_payload_string(root, "verification_uri");
        const gchar *code = clawt_ipc_payload_string(root, "user_code");
        LrgWidget *code_label = clawt_lrg_label((code != NULL) ? code : "?",
                                                "title");
        LrgWidget *open;

        /*
         * The code is the whole interaction, so it is the biggest thing
         * on the dialog: it is about to be read off this screen and
         * typed into another device, quite possibly across a room.
         */
        lrg_widget_set_halign(code_label, LRG_ALIGN_CENTER);
        clawt_lrg_take(content, code_label);
        clawt_lrg_take(content, clawt_lrg_paragraph(
            (uri != NULL) ? uri : "the provider's page", "muted"));

        if (uri != NULL) {
            open = clawt_lrg_button("Open the page", NULL, NULL, NULL);
            g_object_set_data(G_OBJECT(open), "clawt-app", app);
            g_signal_connect_data(open, "clicked", G_CALLBACK(on_open_page),
                                  g_strdup(uri),
                                  (GClosureNotify)(void (*)(void))g_free, 0);
            clawt_lrg_take(content, open);
        }

        dialog = clawt_lrg_dialog(app, "Enter this code", NULL, content,
                                  "Copy the code", FALSE, on_code_copied,
                                  g_strdup((code != NULL) ? code : ""),
                                  g_free);
    } else {
        const gchar *url = clawt_ipc_payload_string(root, "authorize_url");

        /*
         * Opened for them, and still shown: a browser that declines to
         * open -- or opens somewhere they are not looking -- would leave
         * a dialog saying "approve this" with nothing to approve.
         */
        if (url != NULL)
            clawt_lrg_open_uri(app, url);

        clawt_lrg_take(content, clawt_lrg_paragraph(
            (url != NULL) ? url : "(no URL)", NULL));

        dialog = clawt_lrg_dialog(app, "Waiting for you",
                                  "Open this to approve:", content,
                                  "Copy the link", FALSE, on_code_copied,
                                  g_strdup((url != NULL) ? url : ""),
                                  g_free);
    }

    waiting = g_new0(Flow, 1);
    waiting->name = g_strdup(flow->name);
    g_set_weak_pointer(&waiting->dialog, dialog);
    g_object_unref(dialog);
    g_object_unref(content);

    clawt_lrg_request(app, "connector.await",
                      clawt_ipc_payload_new("flow", flow_id, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_awaited, waiting, flow_free);
}

static void
run_flow(ClawtLrgApp *app, const gchar *name)
{
    Flow *flow = g_new0(Flow, 1);

    flow->name = g_strdup(name);
    clawt_lrg_request(app, "connector.begin",
                      clawt_ipc_payload_new("name", name, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_begun, flow, flow_free);
}

/* ── One connector's actions ──────────────────────────────────────── */

typedef struct {
    ClawtLrgApp *app;
    gchar       *name;
    LrgDialog   *menu;    /* weak: the actions dialog */
} Action;

static void
action_free(gpointer data)
{
    Action *action = data;

    g_clear_weak_pointer(&action->menu);
    g_free(action->name);
    g_free(action);
}

static void
close_menu(Action *action)
{
    if (action->menu != NULL)
        lrg_dialog_close(action->menu);
}

static void
on_connect_clicked(LrgButton *button, gpointer user_data)
{
    Action *action = user_data;

    close_menu(action);
    run_flow(action->app, action->name);
}

static void
on_key_stored(ClawtLrgApp *app, JsonNode *reply, const GError *error,
              gpointer user_data)
{
    LrgDialog **dialog = user_data;

    if (reply != NULL) {
        clawt_lrg_toast(app, "Token stored.");

        if (*dialog != NULL)
            lrg_dialog_close(*dialog);
    }

    refresh(app);
}

static void
weak_dialog_free(gpointer data)
{
    LrgDialog **dialog = data;

    g_clear_weak_pointer(dialog);
    g_free(dialog);
}

static gboolean
on_key_response(ClawtLrgApp *app, LrgDialog *dialog, gint response,
                gpointer user_data)
{
    LrgWidget *entry = user_data;
    const gchar *name = g_object_get_data(G_OBJECT(entry), "clawt-connector");
    g_autofree gchar *key = g_strdup(clawt_lrg_entry_text(entry));
    LrgDialog **weak = g_new0(LrgDialog *, 1);

    /*
     * Cleared whether or not it is accepted.  A rejected token is still
     * a live one, and leaving it in a widget keeps it in whatever the
     * toolkit last drew.
     */
    lrg_text_input_set_text(LRG_TEXT_INPUT(entry), "");

    if (*key == '\0') {
        g_free(weak);
        return FALSE;
    }

    g_set_weak_pointer(weak, dialog);
    clawt_lrg_request(app, "connector.key",
                      clawt_ipc_payload_new("name", name, "key", key, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_key_stored, weak,
                      weak_dialog_free);
    memset(key, 0, strlen(key));

    return FALSE;
}

static void
on_key_clicked(LrgButton *button, gpointer user_data)
{
    Action *action = user_data;
    LrgWidget *entry = clawt_lrg_entry(NULL, "Token");

    close_menu(action);

    lrg_text_input_set_password_mode(LRG_TEXT_INPUT(entry), TRUE);
    lrg_widget_set_min_size(entry, 420.0f * (gfloat)action->app->scale,
                            0.0f);
    g_object_set_data_full(G_OBJECT(entry), "clawt-connector",
                           g_strdup(action->name), g_free);

    g_object_unref(clawt_lrg_dialog(
        action->app, "Paste a token",
        "A personal access token works as well as an authorization, and "
        "needs no application registered with the provider. It is stored "
        "0600 and never reaches the agent.",
        entry, "Store", FALSE, on_key_response, entry, NULL));
    g_object_unref(entry);
}

static void
on_renewed(ClawtLrgApp *app, JsonNode *reply, const GError *error,
           gpointer user_data)
{
    if (reply != NULL)
        clawt_lrg_toast(app, "Renewed.");

    refresh(app);
}

static void
on_refresh_clicked(LrgButton *button, gpointer user_data)
{
    Action *action = user_data;

    close_menu(action);
    clawt_lrg_request(action->app, "connector.refresh",
                      clawt_ipc_payload_new("name", action->name, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_renewed, NULL, NULL);
}

static void
on_revoked(ClawtLrgApp *app, JsonNode *reply, const GError *error,
           gpointer user_data)
{
    if (reply != NULL)
        clawt_lrg_toast(app, clawt_lrg_settings_revoked_text(
                                 clawt_ipc_reply_object(reply)));

    refresh(app);
}

static void
on_revoke_confirmed(ClawtLrgApp *app, gpointer user_data)
{
    clawt_lrg_request(app, "connector.revoke",
                      clawt_ipc_payload_new("name", user_data, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_revoked, NULL, NULL);
}

static void
on_revoke_clicked(LrgButton *button, gpointer user_data)
{
    Action *action = user_data;
    g_autofree gchar *title = g_strdup_printf("Revoke %s?", action->name);

    close_menu(action);
    clawt_lrg_confirm(action->app, title,
                      "Every agent loses its tools until it is authorised "
                      "again. Where the provider has an endpoint for it, it "
                      "is told too.",
                      "Revoke", TRUE, on_revoke_confirmed,
                      g_strdup(action->name), g_free);
}

static void
on_integration_removed(ClawtLrgApp *app, JsonNode *reply,
                       const GError *error, gpointer user_data)
{
    if (reply != NULL)
        clawt_lrg_toast(app, "Removed.");

    refresh(app);
}

/*
 * The credential first.  Removing the integration and leaving the
 * token behind would strand a live credential under a name nothing
 * refers to any more -- so the removal waits for the revoke, whichever
 * way the revoke went.
 */
static void
on_revoked_for_removal(ClawtLrgApp *app, JsonNode *reply,
                       const GError *error, gpointer user_data)
{
    clawt_lrg_request(app, "integration.remove",
                      clawt_ipc_payload_new("name", user_data, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_integration_removed, NULL,
                      NULL);
}

static void
on_remove_confirmed(ClawtLrgApp *app, gpointer user_data)
{
    clawt_lrg_request(app, "connector.revoke",
                      clawt_ipc_payload_new("name", user_data, NULL),
                      CLAWT_LRG_QUIET, on_revoked_for_removal,
                      g_strdup(user_data), g_free);
}

static void
on_remove_clicked(LrgButton *button, gpointer user_data)
{
    Action *action = user_data;
    g_autofree gchar *title = g_strdup_printf("Remove %s?", action->name);

    close_menu(action);
    clawt_lrg_confirm(action->app, title,
                      "The credential is revoked first, then the connector "
                      "is removed from every agent.",
                      "Remove", TRUE, on_remove_confirmed,
                      g_strdup(action->name), g_free);
}

/* ── Presenting a big server as a directory ───────────────────────── */

static void
on_directory_saved(ClawtLrgApp *app, JsonNode *reply, const GError *error,
                   gpointer user_data)
{
    if (reply != NULL)
        clawt_lrg_toast(app, "Saved. It applies the next time an agent's "
                             "session starts.");

    refresh(app);
}

/*
 * Saved as soon as it changes, through integration.update like every
 * other connector field.
 */
static void
on_directory_changed(LrgComboBox *combo, gpointer user_data)
{
    Action *action = user_data;
    const gchar *nick = clawt_lrg_combo_id(LRG_WIDGET(combo));

    if (nick == NULL)
        return;

    clawt_lrg_request(action->app, "integration.update",
                      clawt_ipc_payload_new("name", action->name,
                                            "directory", nick, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_directory_saved, NULL, NULL);
}

/*
 * "Present as a directory: Auto / Always / Never", walked from the
 * library's enumeration so a mode added there is offered here at once.
 */
static LrgWidget *
directory_choice(ClawtLrgApp *app, const gchar *name, const gchar *current)
{
    g_autoptr(GPtrArray) nicks = g_ptr_array_new();
    g_autoptr(GPtrArray) labels = g_ptr_array_new();
    LrgWidget *form = clawt_lrg_form_new();
    LrgWidget *combo;
    Action *action = g_new0(Action, 1);
    guint i;

    for (i = 0; i < clawt_tool_directory_mode_count(); i++) {
        g_ptr_array_add(nicks,
                        (gpointer)clawt_tool_directory_mode_nth_nick(i));
        g_ptr_array_add(labels,
                        (gpointer)clawt_tool_directory_mode_nth_label(i));
    }

    g_ptr_array_add(nicks, NULL);
    g_ptr_array_add(labels, NULL);

    combo = clawt_lrg_combo((const gchar *const *)nicks->pdata,
                            (const gchar *const *)labels->pdata,
                            (current != NULL) ? current : "auto");

    action->app = app;
    action->name = g_strdup(name);
    g_signal_connect_data(combo, "changed", G_CALLBACK(on_directory_changed),
                          action,
                          (GClosureNotify)(void (*)(void))action_free, 0);

    clawt_lrg_form_add(form, "Present as a directory", combo,
                       "Three tools that search, describe and run the "
                       "rest, instead of every tool in every prompt.");

    return form;
}

/*
 * Plain buttons in a dialog rather than a menu that runs on selection:
 * GTK learned that a list selecting its first row as it takes focus
 * ran the first entry -- here, Authorize again -- before anybody chose.
 */
static void
on_actions_clicked(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    const gchar *name = g_object_get_data(G_OBJECT(button),
                                          "clawt-connector");
    const gchar *provider = g_object_get_data(G_OBJECT(button),
                                              "clawt-connector-provider");
    LrgWidget *box = clawt_lrg_vbox(8.0f);
    LrgDialog *dialog;
    static const struct {
        const gchar *label;
        const gchar *style;
        GCallback    handler;
    } actions[] = {
        { "Authorize again", "primary", G_CALLBACK(on_connect_clicked) },
        { "Paste a token",   NULL,      G_CALLBACK(on_key_clicked) },
        { "Renew now",       NULL,      G_CALLBACK(on_refresh_clicked) },
        { "Revoke",          "danger",  G_CALLBACK(on_revoke_clicked) },
        { "Remove",          "danger",  G_CALLBACK(on_remove_clicked) }
    };
    GPtrArray *made = g_ptr_array_new();
    guint i;

    lrg_widget_set_min_size(box, 320.0f * (gfloat)app->scale, 0.0f);

    clawt_lrg_take(box, directory_choice(
        app, name, g_object_get_data(G_OBJECT(button),
                                     "clawt-connector-directory")));

    for (i = 0; i < G_N_ELEMENTS(actions); i++) {
        Action *action = g_new0(Action, 1);
        LrgWidget *one;

        action->app = app;
        action->name = g_strdup(name);
        one = clawt_lrg_button(actions[i].label, actions[i].style, NULL,
                               NULL);
        g_signal_connect_data(one, "clicked", actions[i].handler, action,
                              (GClosureNotify)(void (*)(void))action_free,
                              0);
        lrg_widget_set_halign(one, LRG_ALIGN_FILL);
        g_ptr_array_add(made, action);
        clawt_lrg_take(box, one);
    }

    dialog = clawt_lrg_dialog(app, name, provider, box, NULL, FALSE, NULL,
                              NULL, NULL);

    for (i = 0; i < made->len; i++)
        g_set_weak_pointer(&((Action *)g_ptr_array_index(made, i))->menu,
                           dialog);

    g_ptr_array_unref(made);
    g_object_unref(dialog);
    g_object_unref(box);
}

/* ── Adding one ───────────────────────────────────────────────────── */

typedef struct {
    LrgWidget *provider;
    LrgWidget *name;
    LrgWidget *account;
    LrgWidget *client_id;
    LrgWidget *instance;
    LrgWidget *scope;
    LrgWidget *agents;
    LrgDialog *dialog;     /* weak */
    gchar     *chosen;
    gboolean   authorise;
} AddConnector;

static void
add_clear(gpointer data)
{
    AddConnector *add = data;

    g_clear_weak_pointer(&add->dialog);
    g_free(add->chosen);
}

static void
add_release(gpointer data)
{
    g_rc_box_release_full(data, add_clear);
}

static void
on_connector_added(ClawtLrgApp *app, JsonNode *reply, const GError *error,
                   gpointer user_data)
{
    AddConnector *add = user_data;

    if (reply == NULL)
        return;

    if (add->dialog != NULL)
        lrg_dialog_close(add->dialog);

    refresh(app);

    /*
     * Straight into the flow only when there is an application to run
     * it with: without a client id the provider has nothing to identify
     * the request, and starting something that cannot succeed is worse
     * than leaving the next step visible on the list.
     */
    if (add->authorise)
        run_flow(app, add->chosen);
    else
        clawt_lrg_toast(app, "Added. Open it to paste a token, or set a "
                             "client id to authorize.");
}

static gboolean
on_add_response(ClawtLrgApp *app, LrgDialog *dialog, gint response,
                gpointer user_data)
{
    AddConnector *add = user_data;
    g_autoptr(GError) error = NULL;
    const gchar *client_id = clawt_lrg_entry_text(add->client_id);
    JsonNode *payload;

    payload = clawt_lrg_settings_connector_payload(
        clawt_lrg_entry_text(add->name), clawt_lrg_combo_id(add->provider),
        clawt_lrg_combo_id(add->scope),
        clawt_lrg_entry_text(add->account), client_id,
        clawt_lrg_entry_text(add->instance),
        clawt_lrg_entry_text(add->agents), &error);

    if (payload == NULL) {
        clawt_lrg_toast(app, error->message);
        return FALSE;
    }

    g_free(add->chosen);
    add->chosen = g_strdup(clawt_lrg_entry_text(add->name));
    add->authorise = client_id != NULL && *client_id != '\0';

    clawt_lrg_request(app, "integration.add", payload,
                      CLAWT_LRG_REQUEST_NONE, on_connector_added,
                      g_rc_box_acquire(add), add_release);

    return FALSE;
}

static void
on_catalog(ClawtLrgApp *app, JsonNode *reply, const GError *error,
           gpointer user_data)
{
    JsonArray *entries = clawt_lrg_settings_member_array(
        clawt_ipc_reply_object(reply), "connectors");
    g_autoptr(GPtrArray) ids = g_ptr_array_new();
    g_autoptr(GPtrArray) labels = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) scope_ids = g_ptr_array_new();
    g_autoptr(GPtrArray) scope_labels = g_ptr_array_new();
    AddConnector *add;
    LrgWidget *form;
    LrgDialog *dialog;
    guint i;

    if (reply == NULL)
        return;

    for (i = 0; entries != NULL && i < json_array_get_length(entries); i++) {
        JsonObject *entry = json_array_get_object_element(entries, i);
        const gchar *id = clawt_ipc_payload_string(entry, "id");

        if (id == NULL)
            continue;

        g_ptr_array_add(ids, (gpointer)id);
        g_ptr_array_add(labels, clawt_lrg_settings_catalog_label(entry));
    }

    g_ptr_array_add(ids, NULL);
    g_ptr_array_add(labels, NULL);

    add = g_rc_box_new0(AddConnector);
    form = clawt_lrg_form_new();

    add->provider = clawt_lrg_combo((const gchar *const *)ids->pdata,
                                    (const gchar *const *)labels->pdata,
                                    (ids->len > 1)
                                        ? g_ptr_array_index(ids, 0) : NULL);
    add->name = clawt_lrg_entry(NULL, "github-work");
    add->account = clawt_lrg_entry(NULL, "work, personal");
    add->client_id = clawt_lrg_entry(NULL, "leave empty to paste a token");
    add->instance = clawt_lrg_entry(NULL, "if you host it yourself");
    /* walked from ClawtScope, like the integration editor's */
    for (i = 0; i < clawt_lrg_settings_scope_count(); i++) {
        const gchar *nick = clawt_lrg_settings_scope_nth_nick(i);

        g_ptr_array_add(scope_ids, (gpointer)nick);
        g_ptr_array_add(scope_labels,
                        (gpointer)clawt_lrg_settings_scope_label(nick));
    }

    g_ptr_array_add(scope_ids, NULL);
    g_ptr_array_add(scope_labels, NULL);
    add->scope = clawt_lrg_combo((const gchar *const *)scope_ids->pdata,
                                 (const gchar *const *)scope_labels->pdata,
                                 "all");
    add->agents = clawt_lrg_entry(NULL, "kudu, oryx");

    clawt_lrg_form_add(form, "Service", add->provider,
                       "The kind of sign-in is beside each: a device "
                       "connector needs a client id, an api_key one a "
                       "pasted token.");
    clawt_lrg_form_add(form, "Name", add->name, NULL);
    clawt_lrg_form_add(form, "Account", add->account, NULL);
    clawt_lrg_form_add(form, "Client ID", add->client_id, NULL);
    clawt_lrg_form_add(form, "Instance", add->instance, NULL);
    clawt_lrg_form_add(form, "Who gets it", add->scope, NULL);
    clawt_lrg_form_add(form, "Agents, comma separated", add->agents, NULL);
    lrg_widget_set_min_size(form, 500.0f * (gfloat)app->scale, 0.0f);

    dialog = clawt_lrg_dialog(
        app, "Add a connector",
        "clawtilla holds the credential and hands it to the tool server. "
        "Agents get the tools; they never get the token.",
        form, "Add", FALSE, on_add_response, add, add_release);
    g_set_weak_pointer(&add->dialog, dialog);
    g_object_unref(dialog);
    g_object_unref(form);
}

static void
on_add_clicked(LrgButton *button, gpointer user_data)
{
    clawt_lrg_request(user_data, "connector.catalog", NULL,
                      CLAWT_LRG_REQUEST_NONE, on_catalog, NULL, NULL);
}

/*
 * The registry is imported by id, and "Add a connector" asks for the
 * catalogue afresh each time -- so this button's only job is to say
 * what happened: how many, or why not (most often that
 * connectors.registry_enabled is still off).
 */
static void
on_registry(ClawtLrgApp *app, JsonNode *reply, const GError *error,
            gpointer user_data)
{
    g_autofree gchar *text = NULL;

    if (reply == NULL)
        return;

    text = clawt_lrg_settings_imported_text(clawt_ipc_payload_int(
        clawt_ipc_reply_object(reply), "imported", 0));
    clawt_lrg_toast(app, text);
}

static void
on_registry_clicked(LrgButton *button, gpointer user_data)
{
    clawt_lrg_request(user_data, "connector.registry_refresh", NULL,
                      CLAWT_LRG_REQUEST_NONE, on_registry, NULL, NULL);
}

/* ── The list ─────────────────────────────────────────────────────── */

static void
on_list(ClawtLrgApp *app, JsonNode *reply, const GError *error,
        gpointer user_data)
{
    JsonArray *list;
    gint64 min_tools;
    gint64 min_chars;
    guint i;

    if (GPOINTER_TO_UINT(user_data) != connectors.generation ||
        reply == NULL)
        return;

    clawt_lrg_clear(connectors.list);
    list = clawt_lrg_settings_member_array(clawt_ipc_reply_object(reply),
                                           "connectors");
    min_tools = json_object_get_int_member_with_default(
        clawt_ipc_reply_object(reply), "directory_min_tools", 40);
    min_chars = json_object_get_int_member_with_default(
        clawt_ipc_reply_object(reply), "directory_min_chars", 100000);

    if (connectors.min_tools != NULL) {
        g_autofree gchar *tools_text = g_strdup_printf("%" G_GINT64_FORMAT,
                                                       min_tools);
        g_autofree gchar *chars_text = g_strdup_printf("%" G_GINT64_FORMAT,
                                                       min_chars);

        lrg_text_input_set_text(LRG_TEXT_INPUT(connectors.min_tools),
                                tools_text);
        lrg_text_input_set_text(LRG_TEXT_INPUT(connectors.min_chars),
                                chars_text);
    }

    if (list == NULL || json_array_get_length(list) == 0)
        clawt_lrg_take(connectors.list, clawt_lrg_row(
            "No connected accounts",
            "Connect one to give agents a service's tools without giving "
            "them the credential"));

    for (i = 0; list != NULL && i < json_array_get_length(list); i++) {
        JsonObject *entry = json_array_get_object_element(list, i);
        const gchar *name = clawt_ipc_payload_string(entry, "name");
        const gchar *provider = clawt_ipc_payload_string(entry, "provider");
        g_autofree gchar *subtitle =
            clawt_lrg_settings_connector_subtitle(entry);
        ClawtConnectorState state =
            clawt_lrg_settings_connector_state(entry, 0);
        LrgWidget *row;
        LrgWidget *open;

        if (name == NULL)
            continue;

        /*
         * Three states, not two: an expired credential that renews
         * itself is working normally, and drawing it as broken lights
         * the list up every hour and teaches somebody to ignore the one
         * that genuinely is.
         */
        row = clawt_lrg_row(name, subtitle);
        clawt_lrg_row_add_suffix(row, clawt_lrg_badge(
            clawt_connector_state_label(state),
            clawt_lrg_settings_connector_tone(state)));

        open = clawt_lrg_button("Actions…", "flat",
                                G_CALLBACK(on_actions_clicked), app);
        g_object_set_data_full(G_OBJECT(open), "clawt-connector",
                               g_strdup(name), g_free);
        g_object_set_data_full(G_OBJECT(open), "clawt-connector-provider",
                               g_strdup(provider), g_free);
        g_object_set_data_full(G_OBJECT(open), "clawt-connector-directory",
                               g_strdup(clawt_ipc_payload_string(
                                   entry, "directory")), g_free);
        clawt_lrg_row_add_suffix(row, open);

        clawt_lrg_appear(app, row, i);
        clawt_lrg_take(connectors.list, row);

        /*
         * How many tools, and whether agents see them as a directory --
         * libclawt's sentence, the one the other clients draw too.  Its
         * own wrapping line, since a row's subtitle is one.
         */
        {
            g_autofree gchar *tools_line = clawt_mcp_directory_status_label(
                clawt_ipc_payload_string(entry, "directory"),
                json_object_get_int_member_with_default(entry, "tool_count",
                                                        -1),
                json_object_get_int_member_with_default(entry, "tool_chars",
                                                        0),
                min_tools, min_chars);

            clawt_lrg_take(connectors.list,
                           clawt_lrg_paragraph(tools_line, "muted"));
        }
    }

    clawt_lrg_app_wake(app);
}

static void
on_thresholds_saved(ClawtLrgApp *app, JsonNode *reply, const GError *error,
                    gpointer user_data)
{
    if (reply != NULL)
        clawt_lrg_toast(app, "Saved.");

    refresh(app);
}

/*
 * Through connector.settings, which refuses a negative threshold before
 * writing either.  Text that is not a whole number is said so here and
 * not sent, rather than saved as zero.
 */
static void
on_thresholds_save(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    LrgWidget *entries[2];
    const gchar *keys[] = { "directory_min_tools", "directory_min_chars" };
    guint k;

    entries[0] = connectors.min_tools;
    entries[1] = connectors.min_chars;

    json_builder_begin_object(builder);

    for (k = 0; k < G_N_ELEMENTS(keys); k++) {
        const gchar *text = clawt_lrg_entry_text(entries[k]);
        gint64 value = 0;

        if (text == NULL || *text == '\0')
            continue;

        if (!g_ascii_string_to_signed(text, 10, 0, G_MAXINT64, &value,
                                      NULL)) {
            clawt_lrg_toast(app, "A threshold has to be a whole number, "
                                 "zero or more.");
            return;
        }

        json_builder_set_member_name(builder, keys[k]);
        json_builder_add_int_value(builder, value);
    }

    json_builder_end_object(builder);

    clawt_lrg_request(app, "connector.settings",
                      json_builder_get_root(builder), CLAWT_LRG_REQUEST_NONE,
                      on_thresholds_saved, NULL, NULL);
}

static void
refresh(ClawtLrgApp *app)
{
    if (connectors.list == NULL)
        return;

    connectors.generation++;
    clawt_lrg_request(app, "connector.list", NULL, CLAWT_LRG_QUIET, on_list,
                      GUINT_TO_POINTER(connectors.generation), NULL);
}

static LrgWidget *
build(ClawtLrgApp *app)
{
    LrgWidget *page = clawt_lrg_page_new("Connectors",
                                         "Connected accounts");
    LrgWidget *toolbar = clawt_lrg_page_toolbar(page);
    LrgWidget *box = clawt_lrg_vbox(10.0f);

    clawt_lrg_take(toolbar,
                   clawt_lrg_button("Import the registry", "flat",
                                    G_CALLBACK(on_registry_clicked), app));
    clawt_lrg_take(toolbar,
                   clawt_lrg_button("Connect an account", "primary",
                                    G_CALLBACK(on_add_clicked), app));

    clawt_lrg_take(box, clawt_lrg_paragraph(
        "An account clawtilla holds the credential for. Agents get its "
        "tools; the token stays here, is renewed here, and can be "
        "withdrawn here.", "muted"));

    connectors.list = clawt_lrg_vbox(6.0f);
    clawt_lrg_add(box, connectors.list);
    g_object_unref(connectors.list);

    clawt_lrg_take(clawt_lrg_page_body(page), clawt_lrg_card(box));

    /*
     * Servers with many tools: the thresholds `auto` measures a
     * connector against.  Per connector the choice is in its Actions.
     */
    {
        LrgWidget *many = clawt_lrg_vbox(10.0f);
        LrgWidget *form = clawt_lrg_form_new();

        clawt_lrg_take(many, clawt_lrg_section("Servers with many tools"));
        clawt_lrg_take(many, clawt_lrg_paragraph(
            "Past either of these, a connector set to Auto reaches agents "
            "as a directory -- search, describe and call -- unless the "
            "agent's CLI searches tools itself, as Claude Code does.",
            "muted"));

        connectors.min_tools = clawt_lrg_entry("40", "40");
        connectors.min_chars = clawt_lrg_entry("100000", "100000");
        clawt_lrg_form_add(form, "More tools than", connectors.min_tools,
                           NULL);
        clawt_lrg_form_add(form, "More characters of tool definitions than",
                           connectors.min_chars, NULL);
        clawt_lrg_take(many, form);
        clawt_lrg_take(many, clawt_lrg_button(
            "Save", "primary", G_CALLBACK(on_thresholds_save), app));

        clawt_lrg_take(clawt_lrg_page_body(page), clawt_lrg_card(many));
    }

    return page;
}

static const ClawtLrgSettingsPage page_impl = {
    "connectors", "Connectors", build, refresh
};

const ClawtLrgSettingsPage *
clawt_lrg_settings_connectors_page(void)
{
    return &page_impl;
}
