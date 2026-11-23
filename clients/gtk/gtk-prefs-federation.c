/*
 * gtk-prefs-federation.c - Settings: this fleet, linked with others
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Who this fleet is (its id and safety words), the fleets it is linked
 * with -- health, tier, owner, tags, and for each one its grant with
 * where every value came from -- an Invite dialog that shows the invite
 * as a QR code and a paste string, an Accept dialog that previews an
 * invite before anything is linked, and the audit.
 *
 * Mirrors the Federation page of the web client and of clawtilla-lrg;
 * the line under each peer (clawt_fed_peer_summary()), the dot
 * (clawt_fed_health_glyph()) and an audit row (clawt_fed_audit_line())
 * come from libclawt so the three say the same thing.  See
 * docs/federation.org.
 */

#include "clawt-window-private.h"

#define INVITE_QR_SCALE (4)

/* ── Small helpers ───────────────────────────────────────────────── */

static void fill_rules(ClawtWindow *self);

static JsonObject *
payload_or_null(JsonNode *reply)
{
    return reply != NULL ? clawt_payload_of(reply) : NULL;
}

/*
 * A dropdown of the tiers, walked from the library; @nick preselected.
 * @explained puts each tier's summary in its entry -- for the invite and
 * accept dialogs, where it is the choice being made; in a row the label
 * alone, with the summary beside it, or the row's title is squeezed.
 */
static GtkWidget *
tier_dropdown_full(const gchar *nick, gboolean explained)
{
    g_autoptr(GtkStringList) labels = gtk_string_list_new(NULL);
    GtkWidget *dropdown;
    guint selected = 0;
    guint i;

    for (i = 0; i < clawt_fed_tier_count(); i++) {
        ClawtFedTier tier = clawt_fed_tier_nth(i);
        g_autofree gchar *text = explained
            ? g_strdup_printf("%s \342\200\224 %s", clawt_fed_tier_label(tier),
                              clawt_fed_tier_summary(tier))
            : g_strdup(clawt_fed_tier_label(tier));
        gtk_string_list_append(labels, text);
        if (g_strcmp0(clawt_fed_tier_nick(tier), nick) == 0)
            selected = i;
    }
    dropdown = gtk_drop_down_new(G_LIST_MODEL(g_steal_pointer(&labels)), NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(dropdown), selected);
    return dropdown;
}

static GtkWidget *
tier_dropdown(const gchar *nick)
{
    return tier_dropdown_full(nick, TRUE);
}

static const gchar *
tier_nick_at(GtkWidget *dropdown)
{
    guint n = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));

    return clawt_fed_tier_nick(clawt_fed_tier_nth(n));
}

static GtkWidget *
selectable_text(const gchar *text, gboolean monospace)
{
    GtkWidget *label = gtk_label_new(text);

    gtk_label_set_selectable(GTK_LABEL(label), TRUE);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_CHAR);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    if (monospace)
        gtk_widget_add_css_class(label, "monospace");
    return label;
}

/* ── Inviting ────────────────────────────────────────────────────── */

typedef struct {
    ClawtWindow *window;
    GtkWidget   *tier;
    GtkWidget   *confirm;
    GtkWidget   *note;
    GtkWidget   *endpoint;
} InviteForm;

/* "Copy invite": the paste string onto the clipboard, the one thing to send. */
static void
on_invite_shown_response(AdwAlertDialog *dialog, const gchar *response, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *paste = g_object_get_data(G_OBJECT(dialog), "clawt-paste");

    if (g_strcmp0(response, "copy") != 0 || paste == NULL)
        return;
    gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(self)), paste);
    clawt_window_toast(self, "Invite copied. Send it to the other fleet's owner.");
}

static void
show_invite(ClawtWindow *self, JsonObject *invite)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    const gchar *uri = clawt_json_string(invite, "uri", NULL);
    g_autoptr(GdkTexture) qr = uri != NULL ? clawt_gtk_qr_texture(uri, INVITE_QR_SCALE, NULL) : NULL;
    g_autofree gchar *body = g_strdup_printf(
        "Send this to the other fleet's owner -- it is a one-use secret. They "
        "will see this fleet's safety words: %s. It gives them tier %s here.",
        clawt_json_string(invite, "safety_words", "?"),
        clawt_json_string(invite, "tier", "?"));
    AdwDialog *dialog;

    if (qr != NULL) {
        GtkWidget *picture = gtk_picture_new_for_paintable(GDK_PAINTABLE(qr));

        gtk_picture_set_can_shrink(GTK_PICTURE(picture), FALSE);
        gtk_widget_set_halign(picture, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(picture, "clawt-fed-invite-qr");
        gtk_box_append(GTK_BOX(box), picture);
    }
    gtk_box_append(GTK_BOX(box), selectable_text(clawt_json_string(invite, "paste", ""), TRUE));
    {
        g_autofree gchar *where = g_strdup_printf("Reaches this fleet at %s",
                                                  clawt_json_string(invite, "endpoint", "?"));
        GtkWidget *line = gtk_label_new(where);
        gtk_widget_add_css_class(line, "dim-label");
        gtk_widget_add_css_class(line, "caption");
        gtk_box_append(GTK_BOX(box), line);
    }

    dialog = adw_alert_dialog_new("Invite another fleet", body);
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), box);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "close", "Done");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "copy", "Copy invite");
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "copy",
                                             ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "copy");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "close");
    g_object_set_data_full(G_OBJECT(dialog), "clawt-paste",
                           g_strdup(clawt_json_string(invite, "paste", "")), g_free);
    g_signal_connect(dialog, "response", G_CALLBACK(on_invite_shown_response), self);
    adw_dialog_present(dialog, GTK_WIDGET(self));
}

static void
on_invite_response(AdwAlertDialog *dialog, const gchar *response, gpointer user_data)
{
    InviteForm *form = user_data;
    g_autoptr(JsonBuilder) b = NULL;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *note;

    (void)dialog;
    if (g_strcmp0(response, "create") != 0)
        return;
    b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "tier");
    json_builder_add_string_value(b, tier_nick_at(form->tier));
    json_builder_set_member_name(b, "confirm");
    json_builder_add_boolean_value(b, gtk_check_button_get_active(GTK_CHECK_BUTTON(form->confirm)));
    note = gtk_editable_get_text(GTK_EDITABLE(form->note));
    if (note != NULL && *note != '\0') {
        json_builder_set_member_name(b, "note");
        json_builder_add_string_value(b, note);
    }
    {
        const gchar *endpoint = gtk_editable_get_text(GTK_EDITABLE(form->endpoint));
        if (endpoint != NULL && *endpoint != '\0') {
            json_builder_set_member_name(b, "endpoint");
            json_builder_add_string_value(b, endpoint);
        }
    }
    json_builder_end_object(b);
    reply = clawt_window_request(form->window, "federation.invite.create", json_builder_get_root(b));
    if (reply == NULL)
        return;
    show_invite(form->window, payload_or_null(reply));
    clawt_gtk_refresh_settings_federation(form->window);
}

static void
on_invite_clicked(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    InviteForm *form = g_new0(InviteForm, 1);
    AdwDialog *dialog;

    (void)button;
    form->window = self;
    form->tier = tier_dropdown("limited");
    form->confirm = gtk_check_button_new_with_label("Ask me before they are in");
    form->note = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(form->note), "Who it is for (only you see this)");
    /*
     * Where they reach this fleet, prefilled with what the daemon would
     * name -- a fleet with no address of its own can still invite, by
     * naming the tunnel or tailnet address here instead of refusing.
     */
    form->endpoint = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(form->endpoint),
                                   "Where they reach this fleet: https://..., clawtilla://HOST:PORT");
    {
        g_autoptr(JsonNode) status = clawt_window_request(self, "federation.status", NULL);
        const gchar *known = clawt_json_string(payload_or_null(status), "endpoint", NULL);
        if (known != NULL)
            gtk_editable_set_text(GTK_EDITABLE(form->endpoint), known);
    }
    gtk_box_append(GTK_BOX(box), gtk_label_new("What the fleet that accepts gets here:"));
    gtk_box_append(GTK_BOX(box), form->tier);
    gtk_box_append(GTK_BOX(box), form->confirm);
    gtk_box_append(GTK_BOX(box), form->endpoint);
    gtk_box_append(GTK_BOX(box), form->note);

    dialog = adw_alert_dialog_new("Invite another fleet",
                                  "Makes a one-use invite. Anyone holding it can link "
                                  "with this fleet at the tier you choose, until it "
                                  "expires.");
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), box);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "create", "Make invite");
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "create",
                                             ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
    g_signal_connect_data(dialog, "response", G_CALLBACK(on_invite_response), form,
                          (GClosureNotify)(void (*)(void))g_free, 0);
    adw_dialog_present(dialog, GTK_WIDGET(self));
}

/* ── Accepting ───────────────────────────────────────────────────── */

typedef struct {
    ClawtWindow *window;
    GtkWidget   *invite;
    GtkWidget   *preview;
    GtkWidget   *tier;
    GtkWidget   *alias;
    GtkWidget   *tags;
} AcceptForm;

/* The invite, read without being used: who, where, which words. */
static void
on_invite_text_changed(GtkEditable *editable, gpointer user_data)
{
    AcceptForm *form = user_data;
    const gchar *text = gtk_editable_get_text(editable);
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *line = NULL;
    JsonObject *o;

    if (text == NULL || *text == '\0') {
        gtk_label_set_text(GTK_LABEL(form->preview), "");
        return;
    }
    if (form->window->client == NULL)
        return;
    /*
     * Asked of the client directly, not through clawt_window_request():
     * a half-pasted invite is not a failure worth a toast per keystroke.
     * clawt_client_request() answers with the payload itself.
     */
    reply = clawt_client_request(form->window->client, "federation.invite.inspect",
                                 clawt_build_payload("invite", text, NULL), NULL);
    o = reply != NULL && JSON_NODE_HOLDS_OBJECT(reply) ? json_node_get_object(reply) : NULL;
    if (o == NULL) {
        gtk_label_set_text(GTK_LABEL(form->preview), "That is not a usable invite.");
        return;
    }
    line = g_strdup_printf("%s%s%s\nat %s (%s)\nsafety words %s -- check them with its owner\nthey give you: %s%s",
                           clawt_json_string(o, "name", "?"),
                           *clawt_json_string(o, "owner", "") != '\0' ? " \302\267 " : "",
                           clawt_json_string(o, "owner", ""),
                           clawt_json_string(o, "endpoint", "?"),
                           clawt_json_string(o, "path", clawt_json_string(o, "problem", "?")),
                           clawt_json_string(o, "safety_words", "?"),
                           clawt_json_string(o, "tier_summary", ""),
                           clawt_json_boolean(o, "expired", FALSE) ? "\nThis invite has expired." : "");
    gtk_label_set_text(GTK_LABEL(form->preview), line);
    if (*gtk_editable_get_text(GTK_EDITABLE(form->alias)) == '\0')
        gtk_editable_set_text(GTK_EDITABLE(form->alias),
                              clawt_json_string(o, "suggested_alias", ""));
}

static void
on_accept_response(AdwAlertDialog *dialog, const gchar *response, gpointer user_data)
{
    AcceptForm *form = user_data;
    g_autoptr(JsonBuilder) b = NULL;
    g_autoptr(JsonNode) reply = NULL;
    g_auto(GStrv) tags = NULL;
    JsonObject *o;
    guint i;

    (void)dialog;
    if (g_strcmp0(response, "accept") != 0)
        return;
    b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "invite");
    json_builder_add_string_value(b, gtk_editable_get_text(GTK_EDITABLE(form->invite)));
    json_builder_set_member_name(b, "tier");
    json_builder_add_string_value(b, tier_nick_at(form->tier));
    json_builder_set_member_name(b, "alias");
    json_builder_add_string_value(b, gtk_editable_get_text(GTK_EDITABLE(form->alias)));
    json_builder_set_member_name(b, "tags");
    json_builder_begin_array(b);
    tags = g_strsplit_set(gtk_editable_get_text(GTK_EDITABLE(form->tags)), ", ", -1);
    for (i = 0; tags[i] != NULL; i++)
        if (*tags[i] != '\0')
            json_builder_add_string_value(b, tags[i]);
    json_builder_end_array(b);
    json_builder_end_object(b);
    reply = clawt_window_request(form->window, "federation.invite.accept", json_builder_get_root(b));
    o = payload_or_null(reply);
    if (o == NULL)
        return;
    {
        g_autofree gchar *toast = g_strdup_printf(
            "Linked with %s. Check its safety words with its owner: %s.",
            clawt_json_string(o, "label", clawt_json_string(o, "alias", "?")),
            clawt_json_string(o, "safety_words", "?"));
        clawt_window_toast(form->window, toast);
    }
    clawt_gtk_refresh_settings_federation(form->window);
}

static void
on_accept_clicked(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    AcceptForm *form = g_new0(AcceptForm, 1);
    AdwDialog *dialog;

    (void)button;
    form->window = self;
    form->invite = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(form->invite), "clawt-fed1-... or clawtilla://federate?...");
    form->preview = gtk_label_new(NULL);
    gtk_label_set_wrap(GTK_LABEL(form->preview), TRUE);
    gtk_label_set_xalign(GTK_LABEL(form->preview), 0);
    gtk_widget_add_css_class(form->preview, "clawt-fed-preview");
    form->tier = tier_dropdown("limited");
    form->alias = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(form->alias), "What to call it here");
    form->tags = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(form->tags), "Tags, comma-separated");
    g_signal_connect(form->invite, "changed", G_CALLBACK(on_invite_text_changed), form);
    gtk_box_append(GTK_BOX(box), form->invite);
    gtk_box_append(GTK_BOX(box), form->preview);
    gtk_box_append(GTK_BOX(box), gtk_label_new("What that fleet gets here:"));
    gtk_box_append(GTK_BOX(box), form->tier);
    gtk_box_append(GTK_BOX(box), form->alias);
    gtk_box_append(GTK_BOX(box), form->tags);

    dialog = adw_alert_dialog_new("Accept an invite",
                                  "Paste the invite another fleet sent you. Nothing is "
                                  "linked until you accept, and the other fleet's identity "
                                  "is checked against the invite.");
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), box);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", "Cancel");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "accept", "Link");
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "accept",
                                             ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
    g_signal_connect_data(dialog, "response", G_CALLBACK(on_accept_response), form,
                          (GClosureNotify)(void (*)(void))g_free, 0);
    adw_dialog_present(dialog, GTK_WIDGET(self));
}

/* ── One peer's actions ──────────────────────────────────────────── */

static void
peer_request(ClawtWindow *self, const gchar *kind, const gchar *alias, JsonNode *payload)
{
    g_autoptr(JsonNode) reply = clawt_window_request(
        self, kind, payload != NULL ? payload : clawt_build_payload("alias", alias, NULL));

    if (reply != NULL)
        clawt_gtk_refresh_settings_federation(self);
}

static void
on_peer_action(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *alias = g_object_get_data(G_OBJECT(button), "clawt-alias");
    const gchar *kind = g_object_get_data(G_OBJECT(button), "clawt-kind");
    gboolean value = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "clawt-value"));

    if (g_strcmp0(kind, "federation.peer.hide") == 0 || g_strcmp0(kind, "federation.peer.mute") == 0) {
        g_autoptr(JsonBuilder) b = json_builder_new();
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "alias");
        json_builder_add_string_value(b, alias);
        json_builder_set_member_name(b, "value");
        json_builder_add_boolean_value(b, value);
        json_builder_end_object(b);
        peer_request(self, kind, alias, json_builder_get_root(b));
        return;
    }
    if (g_strcmp0(kind, "federation.peer.ping") == 0) {
        g_autoptr(JsonNode) reply = clawt_window_request(self, kind,
                                                         clawt_build_payload("alias", alias, NULL));
        JsonObject *o = payload_or_null(reply);
        if (o != NULL) {
            g_autofree gchar *toast = g_strdup_printf("%s answered in %" G_GINT64_FORMAT " ms.",
                                                      alias, clawt_json_int(o, "ms", 0));
            clawt_window_toast(self, toast);
        }
        clawt_gtk_refresh_settings_federation(self);
        return;
    }
    peer_request(self, kind, alias, NULL);
}

static GtkWidget *
action_button(ClawtWindow *self, const gchar *label, const gchar *kind, const gchar *alias,
              gboolean value, gboolean destructive)
{
    GtkWidget *button = gtk_button_new_with_label(label);

    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    if (destructive)
        gtk_widget_add_css_class(button, "destructive-action");
    g_object_set_data_full(G_OBJECT(button), "clawt-alias", g_strdup(alias), g_free);
    g_object_set_data(G_OBJECT(button), "clawt-kind", (gpointer)kind);
    g_object_set_data(G_OBJECT(button), "clawt-value", GINT_TO_POINTER(value));
    g_signal_connect(button, "clicked", G_CALLBACK(on_peer_action), self);
    return button;
}

static void
on_tier_selected(GObject *dropdown, GParamSpec *pspec, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *alias = g_object_get_data(dropdown, "clawt-alias");
    const gchar *current = g_object_get_data(dropdown, "clawt-tier");
    const gchar *chosen = tier_nick_at(GTK_WIDGET(dropdown));

    (void)pspec;
    if (g_strcmp0(current, chosen) == 0)
        return;
    peer_request(self, "federation.peer.set", alias,
                 clawt_build_payload("alias", alias, "tier", chosen, NULL));
}

/* `field=value` (or `field=` to remove an override), applied on Enter. */
static void
on_grant_apply(AdwEntryRow *row, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *alias = g_object_get_data(G_OBJECT(row), "clawt-alias");
    const gchar *text = gtk_editable_get_text(GTK_EDITABLE(row));
    const gchar *eq = text != NULL ? strchr(text, '=') : NULL;
    g_autoptr(JsonBuilder) b = NULL;
    g_autofree gchar *field = NULL;

    if (eq == NULL) {
        clawt_window_toast(self, "Write it as field=value, e.g. cap.exec=deny.");
        return;
    }
    field = g_strstrip(g_strndup(text, eq - text));
    b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "alias");
    json_builder_add_string_value(b, alias);
    json_builder_set_member_name(b, "grant");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, field);
    json_builder_add_string_value(b, eq + 1);
    json_builder_end_object(b);
    json_builder_end_object(b);
    peer_request(self, "federation.peer.set", alias, json_builder_get_root(b));
}

/* One expander per peer: the line under it, then everything about it. */
static GtkWidget *
peer_row(ClawtWindow *self, JsonObject *peer)
{
    const gchar *alias = clawt_json_string(peer, "alias", "?");
    const gchar *status = clawt_json_string(peer, "status", "active");
    const gchar *tier = clawt_json_string(peer, "tier", "untrusted");
    g_autofree gchar *title = clawt_fed_peer_title(peer);
    g_autofree gchar *summary = clawt_fed_peer_summary(peer);
    g_autoptr(JsonNode) shown = NULL;
    JsonObject *detail;
    GtkWidget *row = adw_expander_row_new();
    GtkWidget *dot = gtk_label_new(clawt_fed_health_glyph(clawt_json_string(peer, "health", NULL)));
    GtkWidget *badge;
    GtkWidget *actions;
    GtkWidget *actions_row;
    GtkWidget *tier_row;
    GtkWidget *dropdown;
    GtkWidget *grant_entry;
    JsonArray *grant;
    guint i;

    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_expander_row_set_subtitle(ADW_EXPANDER_ROW(row), summary);

    gtk_widget_add_css_class(dot, "clawt-fed-health");
    gtk_widget_add_css_class(dot, clawt_json_string(peer, "health_tone", "neutral"));
    adw_expander_row_add_prefix(ADW_EXPANDER_ROW(row), dot);
    badge = gtk_label_new(clawt_json_string(peer, "tier", "?"));
    gtk_widget_add_css_class(badge, "clawt-fed-tier");
    gtk_widget_add_css_class(badge, "caption");
    gtk_widget_add_css_class(badge, clawt_json_string(peer, "tier_tone", "neutral"));
    gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
    adw_expander_row_add_suffix(ADW_EXPANDER_ROW(row), badge);

    /* Actions, in one row: what can be done to this link now. */
    actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    if (g_strcmp0(status, "revoked") != 0) {
        gtk_box_append(GTK_BOX(actions), action_button(self, "Ping", "federation.peer.ping",
                                                       alias, FALSE, FALSE));
        gtk_box_append(GTK_BOX(actions), action_button(
            self, clawt_json_boolean(peer, "hidden", FALSE) ? "Show" : "Hide",
            "federation.peer.hide", alias, !clawt_json_boolean(peer, "hidden", FALSE), FALSE));
        gtk_box_append(GTK_BOX(actions), action_button(
            self, clawt_json_boolean(peer, "muted", FALSE) ? "Unmute" : "Mute",
            "federation.peer.mute", alias, !clawt_json_boolean(peer, "muted", FALSE), FALSE));
        if (g_strcmp0(status, "pending") == 0)
            gtk_box_append(GTK_BOX(actions), action_button(self, "Approve",
                                                           "federation.peer.approve", alias,
                                                           FALSE, FALSE));
        gtk_box_append(GTK_BOX(actions), action_button(self, "Revoke", "federation.peer.revoke",
                                                       alias, FALSE, TRUE));
    } else {
        gtk_box_append(GTK_BOX(actions), action_button(self, "Forget", "federation.peer.forget",
                                                       alias, FALSE, TRUE));
    }
    actions_row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(actions_row), "This link");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(actions_row),
                                clawt_json_string(peer, "endpoint", "It dials this fleet"));
    adw_action_row_add_suffix(ADW_ACTION_ROW(actions_row), actions);
    adw_expander_row_add_row(ADW_EXPANDER_ROW(row), actions_row);

    {
        GtkWidget *words = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(words), "Safety words");
        adw_action_row_set_subtitle(ADW_ACTION_ROW(words),
                                    clawt_json_string(peer, "safety_words", "?"));
        adw_expander_row_add_row(ADW_EXPANDER_ROW(row), words);
    }

    /* The tier, changeable in place. */
    tier_row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(tier_row), "Tier");
    {
        ClawtFedTier t;
        if (clawt_fed_tier_from_nick(tier, &t))
            adw_action_row_set_subtitle(ADW_ACTION_ROW(tier_row), clawt_fed_tier_summary(t));
    }
    dropdown = tier_dropdown_full(tier, FALSE);
    gtk_widget_set_valign(dropdown, GTK_ALIGN_CENTER);
    g_object_set_data_full(G_OBJECT(dropdown), "clawt-alias", g_strdup(alias), g_free);
    g_object_set_data_full(G_OBJECT(dropdown), "clawt-tier", g_strdup(tier), g_free);
    g_signal_connect(dropdown, "notify::selected", G_CALLBACK(on_tier_selected), self);
    adw_action_row_add_suffix(ADW_ACTION_ROW(tier_row), dropdown);
    adw_expander_row_add_row(ADW_EXPANDER_ROW(row), tier_row);

    /* The grant, every field with where its value came from. */
    shown = clawt_window_request(self, "federation.peer.show",
                                 clawt_build_payload("alias", alias, NULL));
    detail = payload_or_null(shown);
    grant = detail != NULL && json_object_has_member(detail, "grant")
        ? json_object_get_array_member(detail, "grant") : NULL;
    for (i = 0; grant != NULL && i < json_array_get_length(grant); i++) {
        JsonObject *f = json_array_get_object_element(grant, i);
        GtkWidget *field = adw_action_row_new();
        GtkWidget *value = gtk_label_new(clawt_json_string(f, "value", ""));
        g_autofree gchar *source = g_strdup(clawt_json_boolean(f, "overridden", FALSE)
                                            ? "set for this fleet"
                                            : clawt_json_string(f, "source", ""));

        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(field), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(field), clawt_json_string(f, "name", "?"));
        adw_action_row_set_subtitle(ADW_ACTION_ROW(field), source);
        gtk_widget_set_tooltip_text(field, clawt_json_string(f, "doc", NULL));
        gtk_widget_add_css_class(value, "monospace");
        adw_action_row_add_suffix(ADW_ACTION_ROW(field), value);
        adw_expander_row_add_row(ADW_EXPANDER_ROW(row), field);
    }
    grant_entry = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(grant_entry),
                                  "Change the grant: field=value (field= to undo)");
    adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(grant_entry), TRUE);
    gtk_widget_add_css_class(grant_entry, "clawt-fed-grant-editor");
    g_object_set_data_full(G_OBJECT(grant_entry), "clawt-alias", g_strdup(alias), g_free);
    g_signal_connect(grant_entry, "apply", G_CALLBACK(on_grant_apply), self);
    adw_expander_row_add_row(ADW_EXPANDER_ROW(row), grant_entry);

    return row;
}

/* ── The page ────────────────────────────────────────────────────── */

void
clawt_gtk_refresh_settings_federation(ClawtWindow *self)
{
    if (self->settings_federation == NULL)
        return;
    if (!clawt_gtk_refresh_enter(self, CLAWT_REFRESH_FEDERATION))
        return;

    do {
        g_autoptr(JsonNode) status = NULL;
        g_autoptr(JsonNode) list = NULL;
        g_autoptr(JsonNode) audit = NULL;
        JsonObject *s;
        JsonArray *peers;
        JsonArray *entries;
        gint64 now = g_get_real_time() / G_USEC_PER_SEC;
        gboolean enabled;
        guint i;

        clawt_gtk_clear_list(GTK_LIST_BOX(self->settings_federation_self));
        clawt_gtk_clear_list(GTK_LIST_BOX(self->settings_federation));
        clawt_gtk_clear_list(GTK_LIST_BOX(self->settings_federation_activity));
        if (self->settings_federation_rules != NULL)
            clawt_gtk_clear_list(GTK_LIST_BOX(self->settings_federation_rules));

        status = clawt_window_request(self, "federation.status", NULL);
        s = payload_or_null(status);
        enabled = clawt_json_boolean(s, "enabled", FALSE);
        gtk_widget_set_sensitive(self->settings_federation_invite, enabled);
        gtk_widget_set_sensitive(self->settings_federation_accept, enabled);

        if (!enabled) {
            GtkWidget *row = adw_action_row_new();
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Federation is off");
            adw_action_row_set_subtitle(ADW_ACTION_ROW(row), clawt_json_string(
                s, "hint", "Set federation.enabled: true and restart the daemon."));
            gtk_list_box_append(GTK_LIST_BOX(self->settings_federation_self), row);
            continue;
        }
        /* office.peers is superseded: the daemon says so while it has entries. */
        if (clawt_json_string(s, "deprecated", NULL) != NULL) {
            GtkWidget *row = adw_action_row_new();
            adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "office.peers is superseded");
            adw_action_row_set_subtitle(ADW_ACTION_ROW(row), clawt_json_string(s, "deprecated", ""));
            gtk_widget_add_css_class(row, "warning");
            gtk_widget_add_css_class(row, "clawt-fed-deprecated");
            gtk_list_box_append(GTK_LIST_BOX(self->settings_federation_self), row);
        }
        {
            static const struct { const gchar *title; const gchar *key; } facts[] = {
                { "This fleet", "name" },
                { "Fleet id", "fleet_id" },
                { "Safety words", "safety_words" },
                { "Reachable at", "endpoint" },
            };
            for (i = 0; i < G_N_ELEMENTS(facts); i++) {
                GtkWidget *row = adw_action_row_new();
                adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
                adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), facts[i].title);
                adw_action_row_set_subtitle(ADW_ACTION_ROW(row), clawt_json_string(
                    s, facts[i].key, "not set -- see federation.public_url"));
                gtk_list_box_append(GTK_LIST_BOX(self->settings_federation_self), row);
            }
        }

        list = clawt_window_request(self, "federation.peer.list", NULL);
        peers = payload_or_null(list) != NULL && json_object_has_member(payload_or_null(list), "peers")
            ? json_object_get_array_member(payload_or_null(list), "peers") : NULL;
        if (peers == NULL || json_array_get_length(peers) == 0) {
            GtkWidget *row = adw_action_row_new();
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "No linked fleets");
            adw_action_row_set_subtitle(ADW_ACTION_ROW(row),
                                        "Invite another fleet, or accept an invite someone "
                                        "sent you. Two daemons you own: clawtilla federation link.");
            gtk_list_box_append(GTK_LIST_BOX(self->settings_federation), row);
        }
        for (i = 0; peers != NULL && i < json_array_get_length(peers); i++)
            gtk_list_box_append(GTK_LIST_BOX(self->settings_federation),
                                peer_row(self, json_array_get_object_element(peers, i)));
        if (self->settings_federation_rules != NULL)
            fill_rules(self);

        audit = clawt_window_request(self, "federation.audit",
                                     json_from_string("{\"limit\":25}", NULL));
        entries = payload_or_null(audit) != NULL && json_object_has_member(payload_or_null(audit), "entries")
            ? json_object_get_array_member(payload_or_null(audit), "entries") : NULL;
        for (i = 0; entries != NULL && i < json_array_get_length(entries); i++) {
            g_autofree gchar *line = clawt_fed_audit_line(json_array_get_object_element(entries, i), now);
            GtkWidget *label = gtk_label_new(line);
            gtk_label_set_xalign(GTK_LABEL(label), 0);
            gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
            gtk_widget_add_css_class(label, "monospace");
            gtk_widget_add_css_class(label, "caption");
            gtk_widget_add_css_class(label, "clawt-fed-activity");
            gtk_widget_set_margin_start(label, 12);
            gtk_widget_set_margin_end(label, 12);
            gtk_widget_set_margin_top(label, 4);
            gtk_widget_set_margin_bottom(label, 4);
            gtk_list_box_append(GTK_LIST_BOX(self->settings_federation_activity), label);
        }
    } while (clawt_gtk_refresh_repeat(self, CLAWT_REFRESH_FEDERATION));
}

void
clawt_gtk_federation_on_event(ClawtWindow *self, ClawtEvent *event)
{
    (void)event;
    clawt_gtk_refresh_settings_federation(self);
}

/* ---- rules (0.72.0) ---------------------------------------------------- */

/* Sends the rules whole; the daemon checks each before it saves any. */
static void
send_rules(ClawtWindow *self, GPtrArray *rules)
{
    g_autoptr(JsonBuilder) b = json_builder_new();
    g_autoptr(JsonNode) reply = NULL;
    guint i;

    json_builder_begin_object(b);
    json_builder_set_member_name(b, "rules");
    json_builder_begin_array(b);
    for (i = 0; i < rules->len; i++)
        json_builder_add_string_value(b, g_ptr_array_index(rules, i));
    json_builder_end_array(b);
    json_builder_end_object(b);
    reply = clawt_window_request(self, "federation.rules", json_builder_get_root(b));
    if (reply != NULL)
        clawt_gtk_refresh_settings_federation(self);
}

static GPtrArray *
current_rules(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = clawt_window_request(self, "federation.rules", NULL);
    JsonObject *o = payload_or_null(reply);
    JsonArray *list = o != NULL && json_object_has_member(o, "rules")
        ? json_object_get_array_member(o, "rules") : NULL;
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    guint i;

    for (i = 0; list != NULL && i < json_array_get_length(list); i++)
        g_ptr_array_add(out, g_strdup(json_array_get_string_element(list, i)));
    return out;
}

static void
on_rule_remove(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(GPtrArray) rules = current_rules(self);
    guint index = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "clawt-rule-index"));

    if (index < rules->len) {
        g_ptr_array_remove_index(rules, index);
        send_rules(self, rules);
    }
}

static void
on_rule_add(AdwEntryRow *row, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(GPtrArray) rules = current_rules(self);
    const gchar *text = gtk_editable_get_text(GTK_EDITABLE(row));

    if (text == NULL || *text == '\0')
        return;
    g_ptr_array_add(rules, g_strdup(text));
    send_rules(self, rules);
}

static void
fill_rules(ClawtWindow *self)
{
    g_autoptr(GPtrArray) rules = current_rules(self);
    GtkWidget *add = adw_entry_row_new();
    guint i;

    for (i = 0; i < rules->len; i++) {
        GtkWidget *row = adw_action_row_new();
        GtkWidget *remove = gtk_button_new_from_icon_name("user-trash-symbolic");

        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), g_ptr_array_index(rules, i));
        gtk_widget_add_css_class(row, "monospace");
        gtk_widget_add_css_class(remove, "flat");
        gtk_widget_set_valign(remove, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text(remove, "Remove this rule");
        g_object_set_data(G_OBJECT(remove), "clawt-rule-index", GUINT_TO_POINTER(i));
        g_signal_connect(remove, "clicked", G_CALLBACK(on_rule_remove), self);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), remove);
        gtk_widget_add_css_class(row, "clawt-fed-rule");
        gtk_list_box_append(GTK_LIST_BOX(self->settings_federation_rules), row);
    }
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(add),
                                  "Add a rule: SELECTOR: field=value; field=value");
    adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(add), TRUE);
    g_signal_connect(add, "apply", G_CALLBACK(on_rule_add), self);
    gtk_list_box_append(GTK_LIST_BOX(self->settings_federation_rules), add);
}

static GtkWidget *
boxed_list(void)
{
    GtkWidget *list = gtk_list_box_new();

    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(list, "boxed-list");
    gtk_widget_set_valign(list, GTK_ALIGN_START);
    return list;
}

GtkWidget *
clawt_gtk_build_federation_page(ClawtWindow *self)
{
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *me = adw_preferences_group_new();
    GtkWidget *peers = adw_preferences_group_new();
    GtkWidget *activity = adw_preferences_group_new();
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(page), "Federation");
    adw_preferences_page_set_name(ADW_PREFERENCES_PAGE(page), "federation");
    adw_preferences_page_set_icon_name(ADW_PREFERENCES_PAGE(page), "network-workgroup-symbolic");

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(me), "This fleet");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(me),
        "Every request between fleets is signed by this fleet's key. Read the "
        "safety words to whoever accepts your invite.");
    self->settings_federation_self = boxed_list();
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(me), self->settings_federation_self);

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(peers), "Linked fleets");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(peers),
        "Each has a tier -- owned, trusted, limited, untrusted -- that presets what "
        "it may do here; open one to see its grant and where each value came from.");
    self->settings_federation_invite = gtk_button_new_with_label("Invite\342\200\246");
    gtk_widget_add_css_class(self->settings_federation_invite, "suggested-action");
    gtk_widget_add_css_class(self->settings_federation_invite, "clawt-fed-invite");
    g_signal_connect(self->settings_federation_invite, "clicked", G_CALLBACK(on_invite_clicked), self);
    self->settings_federation_accept = gtk_button_new_with_label("Accept\342\200\246");
    g_signal_connect(self->settings_federation_accept, "clicked", G_CALLBACK(on_accept_clicked), self);
    gtk_box_append(GTK_BOX(buttons), self->settings_federation_accept);
    gtk_box_append(GTK_BOX(buttons), self->settings_federation_invite);
    gtk_widget_set_valign(buttons, GTK_ALIGN_CENTER);
    adw_preferences_group_set_header_suffix(ADW_PREFERENCES_GROUP(peers), buttons);
    self->settings_federation = boxed_list();
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(peers), self->settings_federation);

    {
        GtkWidget *rules = adw_preferences_group_new();

        adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(rules), "Rules");
        adw_preferences_group_set_description(
            ADW_PREFERENCES_GROUP(rules),
            "Applied in order to every fleet a selector matches, after its tier and "
            "before its own overrides -- e.g. tag:client-acme: reach=listed; cap.exec=deny. "
            "A rule that would not apply is refused as it is written.");
        self->settings_federation_rules = boxed_list();
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(rules), self->settings_federation_rules);
        g_object_set_data(G_OBJECT(page), "clawt-rules-group", rules);
    }

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(activity), "Activity");
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(activity),
                                          "Every request between fleets, newest first.");
    self->settings_federation_activity = boxed_list();
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(activity), self->settings_federation_activity);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(me));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(peers));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(g_object_get_data(G_OBJECT(page),
                                                                     "clawt-rules-group")));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(activity));
    return page;
}
