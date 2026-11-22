/*
 * lrg-office.c - The Office page: who is where, and the office drawn
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Mirrors clients/gtk/gtk-office.c: one office.snapshot read, drawn as
 * the roster, each line libclawt's (clawt_office_roster_line()) and each
 * badge clawt_office_activity_tone().  Re-read on office.presence.  With
 * OFFICE=1 the office is drawn natively above it, in this process
 * (lrg-office-view.c) -- no child, no frames, the same renderer the
 * standalone clawtilla-office uses.
 */

#include "clawt-lrg.h"
#include "clawt-lrg-settings-model.h"

static struct {
    ClawtLrgApp *app;
    LrgWidget   *page;
    LrgWidget   *status;
    LrgWidget   *list;
    LrgWidget   *controls;     /* the on/off button and the world picker */
    gboolean     enabled;
    guint        generation;
} office;

static void request_snapshot(ClawtLrgApp *app);
static void request_worlds(ClawtLrgApp *app);

static void
on_snapshot(ClawtLrgApp *app, JsonNode *reply, const GError *error, gpointer user_data)
{
    JsonObject *root = clawt_ipc_reply_object(reply);
    JsonArray *people;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    guint i;

    if (GPOINTER_TO_UINT(user_data) != office.generation || office.list == NULL ||
        root == NULL)
        return;

    clawt_lrg_clear(office.list);
    if (!clawt_ipc_payload_boolean(root, "enabled", TRUE)) {
        const gchar *why = clawt_ipc_payload_string(root, "why");

        lrg_label_set_text(LRG_LABEL(office.status),
                           why != NULL ? why : "The office is off.");
        clawt_lrg_app_wake(app);
        return;
    }

    now = clawt_ipc_payload_int(root, "now", now);
    {
        const gchar *world = clawt_ipc_payload_string(root, "world");
        g_autofree gchar *status = g_strdup_printf(
            "World: %s.  Every pose is something the daemon saw -- a turn, a tool, a "
            "message, a decision -- and nothing else.", world != NULL ? world : "?");

        lrg_label_set_text(LRG_LABEL(office.status), status);
    }

    people = clawt_lrg_settings_member_array(root, "participants");
    for (i = 0; people != NULL && i < json_array_get_length(people); i++) {
        JsonObject *p = json_array_get_object_element(people, i);
        const gchar *id = clawt_ipc_payload_string(p, "id");
        const gchar *name = clawt_ipc_payload_string(p, "name");
        const gchar *kind = clawt_ipc_payload_string(p, "kind");
        g_autofree gchar *line = clawt_office_roster_line(p, now);
        ClawtOfficeActivity activity = CLAWT_OFFICE_IDLE;
        LrgWidget *row;

        clawt_office_activity_from_nick(clawt_ipc_payload_string(p, "activity"), &activity);
        row = clawt_lrg_row(name != NULL ? name : (id != NULL ? id : "?"), line);
        clawt_lrg_row_add_prefix(row, clawt_lrg_badge(clawt_office_activity_label(activity),
                                                      clawt_office_activity_tone(activity)));
        if (kind != NULL && g_strcmp0(kind, "agent") != 0)
            clawt_lrg_row_add_suffix(row, clawt_lrg_badge(
                g_strcmp0(kind, "grok_bot") == 0 ? "remote"
                : g_strcmp0(kind, "ephemeral") == 0 ? "helper" : kind, "neutral"));
        clawt_lrg_take(office.list, row);
    }
    clawt_lrg_app_wake(app);
}

/* ── Settings: on or off, and which world ───────────────────────── */

static void
on_set(ClawtLrgApp *app, JsonNode *reply, const GError *error, gpointer user_data)
{
    if (reply == NULL)
        return;
    request_worlds(app);
    request_snapshot(app);
}

static void
office_set(ClawtLrgApp *app, JsonNode *payload)
{
    clawt_lrg_request(app, "office.set", payload, CLAWT_LRG_REQUEST_NONE, on_set,
                      NULL, NULL);
}

static void
on_toggle(LrgButton *button, gpointer user_data)
{
    g_autoptr(JsonBuilder) b = json_builder_new();

    json_builder_begin_object(b);
    json_builder_set_member_name(b, "enabled");
    json_builder_add_boolean_value(b, !office.enabled);
    json_builder_end_object(b);
    office_set(user_data, json_builder_get_root(b));
}

static void
on_world(LrgComboBox *combo, gpointer user_data)
{
    const gchar *chosen = clawt_lrg_combo_id(LRG_WIDGET(combo));

    if (chosen != NULL)
        office_set(user_data, clawt_ipc_payload_new("world", chosen, NULL));
}

static void
on_worlds(ClawtLrgApp *app, JsonNode *reply, const GError *error, gpointer user_data)
{
    JsonObject *root = clawt_ipc_reply_object(reply);
    JsonArray *worlds;
    g_autoptr(GPtrArray) ids = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
    LrgWidget *picker;
    guint i;

    if (root == NULL || office.controls == NULL)
        return;
    office.enabled = clawt_ipc_payload_boolean(root, "enabled", FALSE);
    worlds = clawt_lrg_settings_member_array(root, "worlds");
    for (i = 0; worlds != NULL && i < json_array_get_length(worlds); i++) {
        JsonObject *w = json_array_get_object_element(worlds, i);
        const gchar *id = clawt_ipc_payload_string(w, "id");
        const gchar *name = clawt_ipc_payload_string(w, "name");

        g_ptr_array_add(ids, g_strdup(id != NULL ? id : "?"));
        g_ptr_array_add(names, g_strdup(name != NULL ? name : (id != NULL ? id : "?")));
    }
    g_ptr_array_add(ids, NULL);
    g_ptr_array_add(names, NULL);

    clawt_lrg_clear(office.controls);
    clawt_lrg_take(office.controls, clawt_lrg_button(
        office.enabled ? "Turn the office off" : "Turn the office on",
        office.enabled ? "flat" : "primary", G_CALLBACK(on_toggle), app));
    /* connected after the initial selection, so building is not a change */
    picker = clawt_lrg_combo((const gchar *const *)ids->pdata,
                             (const gchar *const *)names->pdata,
                             clawt_ipc_payload_string(root, "current"));
    g_signal_connect(picker, "changed", G_CALLBACK(on_world), app);
    clawt_lrg_take(office.controls, picker);
    clawt_lrg_app_wake(app);
}

static void
request_worlds(ClawtLrgApp *app)
{
    if (office.controls == NULL)
        return;
    clawt_lrg_request(app, "office.worlds", NULL, CLAWT_LRG_QUIET, on_worlds, NULL, NULL);
}

static void
request_snapshot(ClawtLrgApp *app)
{
    if (office.list == NULL)
        return;
    office.generation++;
    clawt_lrg_request(app, "office.snapshot", NULL, CLAWT_LRG_QUIET, on_snapshot,
                      GUINT_TO_POINTER(office.generation), NULL);
}

static LrgWidget *
build(ClawtLrgApp *app)
{
    LrgWidget *card = clawt_lrg_vbox(8.0f);

    office.app = app;
    office.page = clawt_lrg_page_new("Office", "Who is where, from what the daemon saw");

    /* on or off, and which world: the Office settings live with the office */
    office.controls = clawt_lrg_hbox(8.0f);
    clawt_lrg_add(clawt_lrg_page_toolbar(office.page), office.controls);
    g_object_unref(office.controls);

    /* the drawn office first, when this build has one */
    clawt_lrg_office_view_add(app, clawt_lrg_page_body(office.page));

    office.status = clawt_lrg_label("", "muted");
    clawt_lrg_add(card, office.status);
    g_object_unref(office.status);
    office.list = clawt_lrg_vbox(6.0f);
    clawt_lrg_add(card, office.list);
    g_object_unref(office.list);
    clawt_lrg_take(clawt_lrg_page_body(office.page), clawt_lrg_card(card));
    return office.page;
}

static void
refresh(ClawtLrgApp *app)
{
    request_worlds(app);
    request_snapshot(app);
}

static void
event(ClawtLrgApp *app, ClawtEvent *event)
{
    const gchar *kind = clawt_event_get_kind(event);

    if (office.page == NULL || app->page != CLAWT_PAGE_OFFICE)
        return;
    clawt_lrg_office_view_event(app, event);
    if (g_strcmp0(kind, "office.presence") == 0 || g_strcmp0(kind, "agent.created") == 0 ||
        g_strcmp0(kind, "agent.removed") == 0)
        request_snapshot(app);
}

static const ClawtLrgPage page_impl = {
    CLAWT_PAGE_OFFICE, build, refresh, event
};

const ClawtLrgPage *
clawt_lrg_office_page(void)
{
    return &page_impl;
}

#ifndef CLAWT_HAVE_OFFICE

void
clawt_lrg_office_view_add(ClawtLrgApp *app, LrgWidget *parent)
{
    (void)app;
    clawt_lrg_take(parent, clawt_lrg_label(
        "The office itself is drawn in an OFFICE=1 build (`make OFFICE=1 "
        "LRG_BACKEND=1`). The roster below is the same information.", "muted"));
}

void
clawt_lrg_office_view_event(ClawtLrgApp *app, ClawtEvent *event)
{
    (void)app;
    (void)event;
}

#endif /* !CLAWT_HAVE_OFFICE */
