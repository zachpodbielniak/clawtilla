/*
 * lrg-settings-federation.c - Settings: this fleet, linked with others
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Mirrors the Federation page of clients/gtk/gtk-prefs-federation.c and
 * the web client's /settings/federation: who this fleet is, the linked
 * fleets with their health, tier, owner and tags, an Invite and an
 * Accept dialog, each peer's grant with where every value came from,
 * and the audit.  A peer's line (clawt_fed_peer_summary()), its dot
 * (clawt_fed_health_glyph()) and an audit row (clawt_fed_audit_line())
 * come from libclawt so the three clients say the same thing.
 *
 * Every request is clawt_lrg_request(): this client may never wait on a
 * reply inside a frame.
 */

#include "clawt-lrg.h"

#include <string.h>

typedef struct {
    LrgWidget *me;
    LrgWidget *list;
    LrgWidget *activity;
    LrgWidget *rules;        /* federation.rules, editable (0.72.0) */
    LrgWidget *rule_entry;
    GPtrArray *rule_text;    /* what the last answer said, for add/remove */
    guint      generation;
} Federation;

static Federation fed;

static void refresh(ClawtLrgApp *app);
static void on_rules(ClawtLrgApp *app, JsonNode *reply, const GError *error, gpointer user_data);

/* The tier ids and labels, walked from the library. */
static LrgWidget *
tier_combo(const gchar *selected, gboolean explained)
{
    g_autoptr(GPtrArray) ids = g_ptr_array_new();
    g_autoptr(GPtrArray) labels = g_ptr_array_new_with_free_func(g_free);
    guint i;

    for (i = 0; i < clawt_fed_tier_count(); i++) {
        ClawtFedTier tier = clawt_fed_tier_nth(i);
        g_ptr_array_add(ids, (gpointer)clawt_fed_tier_nick(tier));
        g_ptr_array_add(labels, explained
                        ? g_strdup_printf("%s -- %s", clawt_fed_tier_label(tier),
                                          clawt_fed_tier_summary(tier))
                        : g_strdup(clawt_fed_tier_label(tier)));
    }
    g_ptr_array_add(ids, NULL);
    g_ptr_array_add(labels, NULL);
    return clawt_lrg_combo((const gchar *const *)ids->pdata,
                           (const gchar *const *)labels->pdata, selected);
}

static void
on_done(ClawtLrgApp *app, JsonNode *reply, const GError *error, gpointer user_data)
{
    if (reply != NULL && user_data != NULL)
        clawt_lrg_toast(app, user_data);
    refresh(app);
}

/* ── Inviting ─────────────────────────────────────────────────────── */

typedef struct {
    LrgWidget *tier;
    LrgWidget *confirm;
    LrgWidget *endpoint;
    LrgWidget *note;
} InviteForm;

static void
invite_form_free(gpointer data)
{
    InviteForm *f = data;

    g_clear_object(&f->tier);
    g_clear_object(&f->confirm);
    g_clear_object(&f->endpoint);
    g_clear_object(&f->note);
    g_free(f);
}

static void
on_invite_made(ClawtLrgApp *app, JsonNode *reply, const GError *error, gpointer user_data)
{
    JsonObject *o = clawt_ipc_reply_object(reply);
    g_autofree gchar *body = NULL;

    if (o == NULL)
        return;
    body = g_strdup_printf("Send this to the other fleet's owner -- it is a one-use "
                           "secret. They will see this fleet's safety words: %s. It "
                           "gives them tier %s here, and reaches this fleet at %s.",
                           clawt_ipc_payload_string(o, "safety_words"),
                           clawt_ipc_payload_string(o, "tier"),
                           clawt_ipc_payload_string(o, "endpoint"));
    clawt_lrg_show_text(app, "Your invite", body, clawt_ipc_payload_string(o, "paste"));
    refresh(app);
}

static gboolean
on_invite_response(ClawtLrgApp *app, LrgDialog *dialog, gint response, gpointer user_data)
{
    InviteForm *f = user_data;
    g_autoptr(JsonBuilder) b = NULL;
    const gchar *endpoint;
    const gchar *note;

    if (response != 1)
        return TRUE;
    b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "tier");
    json_builder_add_string_value(b, clawt_lrg_combo_id(f->tier));
    json_builder_set_member_name(b, "confirm");
    json_builder_add_boolean_value(b, clawt_lrg_switch_active(f->confirm));
    endpoint = clawt_lrg_entry_text(f->endpoint);
    if (endpoint != NULL && *endpoint != '\0') {
        json_builder_set_member_name(b, "endpoint");
        json_builder_add_string_value(b, endpoint);
    }
    note = clawt_lrg_entry_text(f->note);
    if (note != NULL && *note != '\0') {
        json_builder_set_member_name(b, "note");
        json_builder_add_string_value(b, note);
    }
    json_builder_end_object(b);
    clawt_lrg_request(app, "federation.invite.create", json_builder_get_root(b),
                      CLAWT_LRG_REQUEST_NONE, on_invite_made, NULL, NULL);
    return TRUE;
}

static void
on_status_for_invite(ClawtLrgApp *app, JsonNode *reply, const GError *error, gpointer user_data)
{
    JsonObject *o = clawt_ipc_reply_object(reply);
    InviteForm *f = g_new0(InviteForm, 1);
    LrgWidget *form = clawt_lrg_form_new();
    const gchar *known = clawt_ipc_payload_string(o, "endpoint");

    f->tier = g_object_ref(tier_combo("limited", TRUE));
    f->confirm = g_object_ref(clawt_lrg_switch("Ask me before they are in", FALSE));
    f->endpoint = g_object_ref(clawt_lrg_entry(known, "https://... or clawtilla://HOST:PORT"));
    f->note = g_object_ref(clawt_lrg_entry(NULL, "who it is for (only you see this)"));
    clawt_lrg_form_add(form, "They get here", f->tier, NULL);
    clawt_lrg_form_add(form, "Approval", f->confirm, NULL);
    clawt_lrg_form_add(form, "Reached at", f->endpoint,
                       "Where the other fleet reaches this one");
    clawt_lrg_form_add(form, "Note", f->note, NULL);
    g_object_unref(clawt_lrg_dialog(app, "Invite another fleet",
                                    "Makes a one-use invite. Anyone holding it can link with "
                                    "this fleet at the tier you choose, until it expires.",
                                    form, "Make invite", FALSE, on_invite_response, f,
                                    invite_form_free));
    g_object_unref(form);
}

static void
on_invite_clicked(LrgButton *button, gpointer user_data)
{
    clawt_lrg_request(user_data, "federation.status", NULL, CLAWT_LRG_REQUEST_NONE,
                      on_status_for_invite, NULL, NULL);
}

/* ── Accepting ────────────────────────────────────────────────────── */

typedef struct {
    LrgWidget *invite;
    LrgWidget *tier;
    LrgWidget *alias;
    LrgWidget *tags;
    LrgWidget *preview;
    ClawtLrgApp *app;
} AcceptForm;

static void
accept_form_free(gpointer data)
{
    AcceptForm *f = data;

    g_clear_object(&f->invite);
    g_clear_object(&f->tier);
    g_clear_object(&f->alias);
    g_clear_object(&f->tags);
    g_clear_object(&f->preview);
    g_free(f);
}

/* What the pasted invite says, read without using it. */
static void
on_inspected(ClawtLrgApp *app, JsonNode *reply, const GError *error, gpointer user_data)
{
    /* The paragraph, held by the request: the dialog may close first. */
    LrgWidget *preview = user_data;
    JsonObject *o = clawt_ipc_reply_object(reply);
    g_autofree gchar *line = NULL;

    if (o == NULL) {
        clawt_lrg_paragraph_set_text(preview, error != NULL ? error->message
                                                               : "That is not a usable invite.");
    } else {
        line = g_strdup_printf("%s%s%s at %s (%s). Safety words %s -- check them with its "
                               "owner. They give you: %s%s",
                               clawt_ipc_payload_string(o, "name"),
                               *(clawt_ipc_payload_string(o, "owner") != NULL
                                 ? clawt_ipc_payload_string(o, "owner") : "") != '\0' ? " \302\267 " : "",
                               clawt_ipc_payload_string(o, "owner") != NULL
                                   ? clawt_ipc_payload_string(o, "owner") : "",
                               clawt_ipc_payload_string(o, "endpoint"),
                               clawt_ipc_payload_string(o, "path") != NULL
                                   ? clawt_ipc_payload_string(o, "path")
                                   : clawt_ipc_payload_string(o, "problem"),
                               clawt_ipc_payload_string(o, "safety_words"),
                               clawt_ipc_payload_string(o, "tier_summary"),
                               clawt_ipc_payload_boolean(o, "expired", FALSE)
                                   ? " This invite has expired." : "");
        clawt_lrg_paragraph_set_text(preview, line);
    }
    clawt_lrg_app_wake(app);
}

static void
on_preview_clicked(LrgButton *button, gpointer user_data)
{
    AcceptForm *f = user_data;

    clawt_lrg_request(f->app, "federation.invite.inspect",
                      clawt_ipc_payload_new("invite", clawt_lrg_entry_text(f->invite), NULL),
                      CLAWT_LRG_QUIET, on_inspected, g_object_ref(f->preview), g_object_unref);
}

static void
on_accepted(ClawtLrgApp *app, JsonNode *reply, const GError *error, gpointer user_data)
{
    JsonObject *o = clawt_ipc_reply_object(reply);
    g_autofree gchar *line = NULL;

    if (o == NULL)
        return;
    line = g_strdup_printf("Linked with %s. Check its safety words with its owner: %s.",
                           clawt_ipc_payload_string(o, "label") != NULL
                               ? clawt_ipc_payload_string(o, "label")
                               : clawt_ipc_payload_string(o, "alias"),
                           clawt_ipc_payload_string(o, "safety_words"));
    clawt_lrg_toast(app, line);
    refresh(app);
}

static gboolean
on_accept_response(ClawtLrgApp *app, LrgDialog *dialog, gint response, gpointer user_data)
{
    AcceptForm *f = user_data;
    g_autoptr(JsonBuilder) b = NULL;
    g_auto(GStrv) tags = NULL;
    guint i;

    if (response != 1)
        return TRUE;
    b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "invite");
    json_builder_add_string_value(b, clawt_lrg_entry_text(f->invite));
    json_builder_set_member_name(b, "tier");
    json_builder_add_string_value(b, clawt_lrg_combo_id(f->tier));
    json_builder_set_member_name(b, "alias");
    json_builder_add_string_value(b, clawt_lrg_entry_text(f->alias));
    json_builder_set_member_name(b, "tags");
    json_builder_begin_array(b);
    tags = g_strsplit_set(clawt_lrg_entry_text(f->tags) != NULL
                          ? clawt_lrg_entry_text(f->tags) : "", ", ", -1);
    for (i = 0; tags[i] != NULL; i++)
        if (*tags[i] != '\0')
            json_builder_add_string_value(b, tags[i]);
    json_builder_end_array(b);
    json_builder_end_object(b);
    clawt_lrg_request(app, "federation.invite.accept", json_builder_get_root(b),
                      CLAWT_LRG_REQUEST_NONE, on_accepted, NULL, NULL);
    return TRUE;
}

static void
on_accept_clicked(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    AcceptForm *f = g_new0(AcceptForm, 1);
    LrgWidget *form = clawt_lrg_form_new();

    f->app = app;
    f->invite = g_object_ref(clawt_lrg_entry(NULL, "clawt-fed1-... or clawtilla://federate?..."));
    f->preview = g_object_ref(clawt_lrg_paragraph("Paste an invite, then Read it.", "muted"));
    f->tier = g_object_ref(tier_combo("limited", TRUE));
    f->alias = g_object_ref(clawt_lrg_entry(NULL, "what to call it here"));
    f->tags = g_object_ref(clawt_lrg_entry(NULL, "comma-separated"));
    clawt_lrg_form_add(form, "Invite", f->invite, NULL);
    clawt_lrg_form_add(form, "What it says",
                       clawt_lrg_button("Read it", NULL, G_CALLBACK(on_preview_clicked), f), NULL);
    clawt_lrg_take(form, g_object_ref(f->preview));
    clawt_lrg_form_add(form, "They get here", f->tier, NULL);
    clawt_lrg_form_add(form, "Alias", f->alias, "Empty: from the fleet's name");
    clawt_lrg_form_add(form, "Tags", f->tags, NULL);
    g_object_unref(clawt_lrg_dialog(app, "Accept an invite",
                                    "Nothing is linked until you accept, and the other "
                                    "fleet's identity is checked against the invite.",
                                    form, "Link", FALSE, on_accept_response, f,
                                    accept_form_free));
    g_object_unref(form);
}

/* ── One peer ─────────────────────────────────────────────────────── */

static void
on_peer_action(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    const gchar *alias = g_object_get_data(G_OBJECT(button), "clawt-alias");
    const gchar *kind = g_object_get_data(G_OBJECT(button), "clawt-kind");
    gboolean value = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "clawt-value"));
    JsonNode *payload;

    if (g_strcmp0(kind, "federation.peer.hide") == 0 || g_strcmp0(kind, "federation.peer.mute") == 0) {
        g_autoptr(JsonBuilder) b = json_builder_new();
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "alias");
        json_builder_add_string_value(b, alias);
        json_builder_set_member_name(b, "value");
        json_builder_add_boolean_value(b, value);
        json_builder_end_object(b);
        payload = json_builder_get_root(b);
    } else {
        payload = clawt_ipc_payload_new("alias", alias, NULL);
    }
    clawt_lrg_request(app, kind, payload, CLAWT_LRG_REQUEST_NONE, on_done,
                      g_strcmp0(kind, "federation.peer.ping") == 0 ? (gpointer)"It answered." : NULL,
                      NULL);
}

static LrgWidget *
peer_button(ClawtLrgApp *app, const gchar *label, const gchar *style, const gchar *kind,
            const gchar *alias, gboolean value)
{
    LrgWidget *button = clawt_lrg_button(label, style, G_CALLBACK(on_peer_action), app);

    g_object_set_data_full(G_OBJECT(button), "clawt-alias", g_strdup(alias), g_free);
    g_object_set_data(G_OBJECT(button), "clawt-kind", (gpointer)kind);
    g_object_set_data(G_OBJECT(button), "clawt-value", GINT_TO_POINTER(value));
    return button;
}

/* The grant, every field with its source, in a copyable dialog. */
static void
on_shown(ClawtLrgApp *app, JsonNode *reply, const GError *error, gpointer user_data)
{
    JsonObject *o = clawt_ipc_reply_object(reply);
    JsonArray *grant = o != NULL && json_object_has_member(o, "grant")
        ? json_object_get_array_member(o, "grant") : NULL;
    g_autoptr(GString) text = g_string_new(NULL);
    g_autofree gchar *title = clawt_fed_peer_title(o);
    g_autofree gchar *body = NULL;
    guint i;

    if (o == NULL)
        return;
    for (i = 0; grant != NULL && i < json_array_get_length(grant); i++) {
        JsonObject *f = json_array_get_object_element(grant, i);
        g_string_append_printf(text, "%-22s %-10s %s\n", clawt_ipc_payload_string(f, "name"),
                               clawt_ipc_payload_string(f, "value"),
                               clawt_ipc_payload_boolean(f, "overridden", FALSE)
                                   ? "set for this fleet" : clawt_ipc_payload_string(f, "source"));
    }
    body = g_strdup_printf("Safety words %s. Reached at %s. Change a field with "
                           "clawtilla federation set %s FIELD=VALUE.",
                           clawt_ipc_payload_string(o, "safety_words"),
                           clawt_ipc_payload_string(o, "endpoint") != NULL
                               ? clawt_ipc_payload_string(o, "endpoint") : "(it dials this fleet)",
                           clawt_ipc_payload_string(o, "alias"));
    clawt_lrg_show_text(app, title, body, text->str);
}

static void
on_details_clicked(LrgButton *button, gpointer user_data)
{
    const gchar *alias = g_object_get_data(G_OBJECT(button), "clawt-alias");

    clawt_lrg_request(user_data, "federation.peer.show",
                      clawt_ipc_payload_new("alias", alias, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_shown, NULL, NULL);
}

static void
on_tier_changed(LrgComboBox *combo, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    const gchar *alias = g_object_get_data(G_OBJECT(combo), "clawt-alias");

    clawt_lrg_request(app, "federation.peer.set",
                      clawt_ipc_payload_new("alias", alias, "tier", clawt_lrg_combo_id(LRG_WIDGET(combo)), NULL),
                      CLAWT_LRG_REQUEST_NONE, on_done, (gpointer)"Tier changed.", NULL);
}

LrgWidget *
clawt_lrg_settings_federation_peer_row(ClawtLrgApp *app, JsonObject *peer)
{
    const gchar *alias = clawt_ipc_payload_string(peer, "alias");
    const gchar *status = clawt_ipc_payload_string(peer, "status");
    g_autofree gchar *title = clawt_fed_peer_title(peer);
    g_autofree gchar *summary = clawt_fed_peer_summary(peer);
    LrgWidget *row = clawt_lrg_row(title, summary);
    LrgWidget *actions = clawt_lrg_hbox(6.0f);
    LrgWidget *dot = clawt_lrg_badge(clawt_fed_health_glyph(clawt_ipc_payload_string(peer, "health")),
                                     clawt_fed_badge_tone(clawt_ipc_payload_string(peer, "health_tone")));
    LrgWidget *tier = tier_combo(clawt_ipc_payload_string(peer, "tier"), FALSE);
    LrgWidget *details;

    lrg_widget_add_class(dot, "clawt-fed-health");
    clawt_lrg_row_add_prefix(row, dot);
    {
        LrgWidget *badge = clawt_lrg_badge(clawt_ipc_payload_string(peer, "tier"),
                                           clawt_fed_badge_tone(clawt_ipc_payload_string(peer, "tier_tone")));
        lrg_widget_add_class(badge, "clawt-fed-tier");
        clawt_lrg_row_add_prefix(row, badge);
    }

    if (g_strcmp0(status, "revoked") != 0) {
        g_object_set_data_full(G_OBJECT(tier), "clawt-alias", g_strdup(alias), g_free);
        g_signal_connect(tier, "changed", G_CALLBACK(on_tier_changed), app);
        clawt_lrg_add(actions, tier);
        clawt_lrg_take(actions, peer_button(app, "Ping", NULL, "federation.peer.ping", alias, FALSE));
        clawt_lrg_take(actions, peer_button(
            app, clawt_ipc_payload_boolean(peer, "hidden", FALSE) ? "Show" : "Hide", NULL,
            "federation.peer.hide", alias, !clawt_ipc_payload_boolean(peer, "hidden", FALSE)));
        clawt_lrg_take(actions, peer_button(
            app, clawt_ipc_payload_boolean(peer, "muted", FALSE) ? "Unmute" : "Mute", NULL,
            "federation.peer.mute", alias, !clawt_ipc_payload_boolean(peer, "muted", FALSE)));
        if (g_strcmp0(status, "pending") == 0)
            clawt_lrg_take(actions, peer_button(app, "Approve", "primary",
                                                "federation.peer.approve", alias, FALSE));
        clawt_lrg_take(actions, peer_button(app, "Revoke", "danger", "federation.peer.revoke",
                                            alias, FALSE));
    } else {
        clawt_lrg_take(actions, peer_button(app, "Forget", "danger", "federation.peer.forget",
                                            alias, FALSE));
    }
    g_object_unref(tier);
    details = clawt_lrg_button("Grant", NULL, G_CALLBACK(on_details_clicked), app);
    lrg_widget_add_class(details, "clawt-fed-grant-editor");
    g_object_set_data_full(G_OBJECT(details), "clawt-alias", g_strdup(alias), g_free);
    clawt_lrg_take(actions, details);
    /*
     * add_suffix() takes the reference (it is clawt_lrg_take()); this
     * also unreffed it, so the row held a freed box: no buttons, a
     * collapsed row, criticals, and a segfault walking the tree.
     */
    clawt_lrg_row_add_suffix(row, actions);
    return row;
}

/* ── The page ─────────────────────────────────────────────────────── */

static void
on_audit(ClawtLrgApp *app, JsonNode *reply, const GError *error, gpointer user_data)
{
    JsonObject *o = clawt_ipc_reply_object(reply);
    JsonArray *entries = o != NULL && json_object_has_member(o, "entries")
        ? json_object_get_array_member(o, "entries") : NULL;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    guint i;

    if (GPOINTER_TO_UINT(user_data) != fed.generation)
        return;
    clawt_lrg_clear(fed.activity);
    if (entries == NULL || json_array_get_length(entries) == 0)
        clawt_lrg_take(fed.activity, clawt_lrg_paragraph("Nothing yet.", "muted"));
    for (i = 0; entries != NULL && i < json_array_get_length(entries); i++) {
        g_autofree gchar *line = clawt_fed_audit_line(json_array_get_object_element(entries, i), now);
        LrgWidget *label = clawt_lrg_one_line(line, "mono");
        lrg_widget_add_class(label, "clawt-fed-activity");
        clawt_lrg_take(fed.activity, label);
    }
    clawt_lrg_app_wake(app);
}

static void
on_peers(ClawtLrgApp *app, JsonNode *reply, const GError *error, gpointer user_data)
{
    JsonObject *o = clawt_ipc_reply_object(reply);
    JsonArray *peers = o != NULL && json_object_has_member(o, "peers")
        ? json_object_get_array_member(o, "peers") : NULL;
    guint i;

    if (GPOINTER_TO_UINT(user_data) != fed.generation)
        return;
    clawt_lrg_clear(fed.list);
    if (peers == NULL || json_array_get_length(peers) == 0)
        clawt_lrg_take(fed.list, clawt_lrg_row(
            "No linked fleets",
            "Invite another fleet, or accept an invite someone sent you. Two "
            "daemons you own: clawtilla federation link."));
    for (i = 0; peers != NULL && i < json_array_get_length(peers); i++) {
        LrgWidget *row = clawt_lrg_settings_federation_peer_row(app, json_array_get_object_element(peers, i));
        clawt_lrg_appear(app, row, i);
        clawt_lrg_take(fed.list, row);
    }
    clawt_lrg_app_wake(app);
}

static void
on_status(ClawtLrgApp *app, JsonNode *reply, const GError *error, gpointer user_data)
{
    JsonObject *o = clawt_ipc_reply_object(reply);

    if (GPOINTER_TO_UINT(user_data) != fed.generation)
        return;
    clawt_lrg_clear(fed.me);
    if (!clawt_ipc_payload_boolean(o, "enabled", FALSE)) {
        clawt_lrg_take(fed.me, clawt_lrg_row(
            "Federation is off",
            clawt_ipc_payload_string(o, "hint") != NULL ? clawt_ipc_payload_string(o, "hint")
                : "Set federation.enabled: true and restart the daemon."));
        clawt_lrg_app_wake(app);
        return;
    }
    /* office.peers is superseded: the daemon says so while it has entries. */
    if (clawt_ipc_payload_string(o, "deprecated") != NULL) {
        LrgWidget *notice = clawt_lrg_paragraph(clawt_ipc_payload_string(o, "deprecated"), "muted");
        lrg_widget_set_name(notice, "clawt-fed-deprecated");
        clawt_lrg_take(fed.me, notice);
    }
    clawt_lrg_take(fed.me, clawt_lrg_row("This fleet", clawt_ipc_payload_string(o, "name")));
    clawt_lrg_take(fed.me, clawt_lrg_row("Fleet id", clawt_ipc_payload_string(o, "fleet_id")));
    clawt_lrg_take(fed.me, clawt_lrg_row("Safety words", clawt_ipc_payload_string(o, "safety_words")));
    clawt_lrg_take(fed.me, clawt_lrg_row(
        "Reachable at", clawt_ipc_payload_string(o, "endpoint") != NULL
                            ? clawt_ipc_payload_string(o, "endpoint")
                            : "not set -- see federation.public_url"));
    clawt_lrg_request(app, "federation.peer.list", NULL, CLAWT_LRG_QUIET, on_peers,
                      GUINT_TO_POINTER(fed.generation), NULL);
    clawt_lrg_request(app, "federation.audit",
                      json_from_string("{\"limit\":25}", NULL), CLAWT_LRG_QUIET, on_audit,
                      GUINT_TO_POINTER(fed.generation), NULL);
    clawt_lrg_request(app, "federation.rules", NULL, CLAWT_LRG_QUIET, on_rules,
                      GUINT_TO_POINTER(fed.generation), NULL);
    clawt_lrg_app_wake(app);
}

/* ---- rules (0.72.0) ---------------------------------------------------- */

static void
on_rules_saved(ClawtLrgApp *app, JsonNode *reply, const GError *error, gpointer user_data)
{
    (void)reply;
    /*
     * An added rule leaves the entry once it is saved; a second Add of
     * the text still sitting there wrote the same rule twice.  A refused
     * one stays, to be corrected.
     */
    if (error == NULL && GPOINTER_TO_INT(user_data) && fed.rule_entry != NULL)
        lrg_text_input_set_text(LRG_TEXT_INPUT(fed.rule_entry), "");
    refresh(app);
}

/* Sends the rules whole, without @drop and with @added; the daemon checks each. */
static void
send_rules(ClawtLrgApp *app, gint drop, const gchar *added)
{
    g_autoptr(JsonBuilder) b = json_builder_new();
    guint i;

    json_builder_begin_object(b);
    json_builder_set_member_name(b, "rules");
    json_builder_begin_array(b);
    for (i = 0; fed.rule_text != NULL && i < fed.rule_text->len; i++)
        if ((gint)i != drop)
            json_builder_add_string_value(b, g_ptr_array_index(fed.rule_text, i));
    if (added != NULL && *added != '\0')
        json_builder_add_string_value(b, added);
    json_builder_end_array(b);
    json_builder_end_object(b);
    clawt_lrg_request(app, "federation.rules", json_builder_get_root(b), CLAWT_LRG_REQUEST_NONE,
                      on_rules_saved, GINT_TO_POINTER(added != NULL && *added != '\0'), NULL);
}

static void
on_rule_remove(LrgButton *button, gpointer user_data)
{
    send_rules(user_data, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "clawt-rule-index")),
               NULL);
}

static void
on_rule_add(LrgButton *button, gpointer user_data)
{
    (void)button;
    send_rules(user_data, -1, clawt_lrg_entry_text(fed.rule_entry));
}

static void
on_rules(ClawtLrgApp *app, JsonNode *reply, const GError *error, gpointer user_data)
{
    JsonObject *o = clawt_ipc_reply_object(reply);
    JsonNode *member = o != NULL ? json_object_get_member(o, "rules") : NULL;
    JsonArray *list = member != NULL && JSON_NODE_HOLDS_ARRAY(member)
        ? json_node_get_array(member) : NULL;
    guint i;

    (void)error;
    if (GPOINTER_TO_UINT(user_data) != fed.generation)
        return;
    clawt_lrg_clear(fed.rules);
    g_clear_pointer(&fed.rule_text, g_ptr_array_unref);
    fed.rule_text = g_ptr_array_new_with_free_func(g_free);
    if (list == NULL || json_array_get_length(list) == 0)
        clawt_lrg_take(fed.rules, clawt_lrg_paragraph(
            "No rules: each fleet's tier and overrides decide.", "muted"));
    for (i = 0; list != NULL && i < json_array_get_length(list); i++) {
        const gchar *rule = json_array_get_string_element(list, i);
        LrgWidget *line = clawt_lrg_hbox(8.0f);
        LrgWidget *text = clawt_lrg_label(rule, "mono");
        LrgWidget *remove = clawt_lrg_button("Remove", NULL, G_CALLBACK(on_rule_remove), app);

        g_ptr_array_add(fed.rule_text, g_strdup(rule));
        g_object_set_data(G_OBJECT(remove), "clawt-rule-index", GINT_TO_POINTER((gint)i));
        lrg_widget_set_fill_weight(text, 1.0f);
        lrg_label_set_ellipsize(LRG_LABEL(text), TRUE);
        clawt_lrg_take(line, text);
        clawt_lrg_take(line, remove);
        lrg_widget_set_name(line, "clawt-fed-rule");
        clawt_lrg_take(fed.rules, line);
    }
}

static void
refresh(ClawtLrgApp *app)
{
    if (fed.list == NULL)
        return;
    fed.generation++;
    clawt_lrg_request(app, "federation.status", NULL, CLAWT_LRG_QUIET, on_status,
                      GUINT_TO_POINTER(fed.generation), NULL);
}

void
clawt_lrg_settings_federation_event(ClawtLrgApp *app, ClawtEvent *event)
{
    const gchar *kind = clawt_event_get_kind(event);

    if (kind != NULL && g_str_has_prefix(kind, "federation."))
        refresh(app);
}

static LrgWidget *
build(ClawtLrgApp *app)
{
    LrgWidget *page = clawt_lrg_page_new("Federation", "Other fleets linked with this one");
    LrgWidget *box = clawt_lrg_vbox(10.0f);

    {
        LrgWidget *invite = clawt_lrg_button("Invite\342\200\246", "primary",
                                             G_CALLBACK(on_invite_clicked), app);
        lrg_widget_add_class(invite, "clawt-fed-invite");
        clawt_lrg_take(clawt_lrg_page_toolbar(page),
                       clawt_lrg_button("Accept\342\200\246", NULL,
                                        G_CALLBACK(on_accept_clicked), app));
        clawt_lrg_take(clawt_lrg_page_toolbar(page), invite);
    }

    clawt_lrg_take(box, clawt_lrg_section("This fleet"));
    clawt_lrg_take(box, clawt_lrg_paragraph(
        "Every request between fleets is signed by this fleet's key. Read the "
        "safety words to whoever accepts your invite.", "muted"));
    fed.me = clawt_lrg_vbox(4.0f);
    clawt_lrg_add(box, fed.me);
    g_object_unref(fed.me);

    clawt_lrg_take(box, clawt_lrg_section("Linked fleets"));
    clawt_lrg_take(box, clawt_lrg_paragraph(
        "Each has a tier -- owned, trusted, limited, untrusted -- that presets what "
        "it may do here. Grant shows every field and where its value came from.", "muted"));
    fed.list = clawt_lrg_vbox(6.0f);
    clawt_lrg_add(box, fed.list);
    g_object_unref(fed.list);

    clawt_lrg_take(box, clawt_lrg_section("Rules"));
    clawt_lrg_take(box, clawt_lrg_paragraph(
        "Applied in order to every fleet a selector matches, after its tier and before its "
        "own overrides -- e.g. tag:client-acme: reach=listed; cap.exec=deny. A rule that "
        "would not apply is refused as it is written.", "muted"));
    fed.rules = clawt_lrg_vbox(4.0f);
    clawt_lrg_add(box, fed.rules);
    g_object_unref(fed.rules);
    {
        LrgWidget *line = clawt_lrg_hbox(8.0f);

        fed.rule_entry = clawt_lrg_entry(NULL, "SELECTOR: field=value; field=value");
        lrg_widget_set_fill_weight(fed.rule_entry, 1.0f);
        clawt_lrg_take(line, fed.rule_entry);
        clawt_lrg_take(line, clawt_lrg_button("Add rule", NULL, G_CALLBACK(on_rule_add), app));
        clawt_lrg_take(box, line);
    }

    clawt_lrg_take(box, clawt_lrg_section("Activity"));
    fed.activity = clawt_lrg_vbox(2.0f);
    clawt_lrg_add(box, fed.activity);
    g_object_unref(fed.activity);

    clawt_lrg_take(clawt_lrg_page_body(page), clawt_lrg_card(box));
    return page;
}

static const ClawtLrgSettingsPage page_impl = {
    "federation", "Federation", build, refresh
};

const ClawtLrgSettingsPage *
clawt_lrg_settings_federation_page(void)
{
    return &page_impl;
}
