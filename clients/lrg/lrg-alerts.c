/*
 * lrg-alerts.c - The alerts drawer: what happened, and what needed you
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Mirrors GTK's alerts panel (build_alerts_panel() in clawt-window.c).
 * A condition that arrived on its own -- a refused message, a failed
 * download, an agent erroring -- goes here rather than in a toast,
 * because a toast vanishes after a few seconds and leaves no trace if
 * nobody was looking.  Errors and notices are drawn as cards and counted
 * on the bell; the routine stream is drawn quietly, and only when "Show
 * everything" is on.  Older entries come from the daemon's own event log
 * (`event.list`), on request.
 *
 * What is recorded, how it is worded and what is counted are in
 * lrg-alerts-model.c; this file only draws.
 */

#include "clawt-lrg.h"

typedef struct {
    GPtrArray *alerts;          /* ClawtLrgAlert, newest first */
    LrgWidget *drawer;
    LrgWidget *list;
    LrgWidget *bell;
    LrgWidget *history;
    LrgWidget *show_all_toggle;
    gboolean   alerts_show_all;
    gchar     *scope_agent;     /* nullable: only this agent's */
    gboolean   showing;
} Alerts;

static Alerts alerts_panel;

static void refresh_alerts(ClawtLrgApp *app);

static void
update_bell(ClawtLrgApp *app)
{
    guint unread = clawt_lrg_alerts_unread(alerts_panel.alerts);
    g_autofree gchar *label = NULL;

    if (alerts_panel.bell == NULL)
        return;

    label = (unread > 0) ? g_strdup_printf("Alerts  %u", unread)
                         : g_strdup("Alerts");
    lrg_button_set_text(LRG_BUTTON(alerts_panel.bell), label);

    if (unread > 0)
        lrg_widget_add_class(alerts_panel.bell, "clawt-unread");
    else
        lrg_widget_remove_class(alerts_panel.bell, "clawt-unread");

    clawt_lrg_app_wake(app);
}

static void
on_scope_clicked(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    const gchar *agent = g_object_get_data(G_OBJECT(button), "agent");

    /* the agent's name is the one control the scope needs */
    g_free(alerts_panel.scope_agent);
    alerts_panel.scope_agent = g_strdup(agent);
    refresh_alerts(app);
}

static void
on_scope_cleared(LrgButton *button, gpointer user_data)
{
    g_clear_pointer(&alerts_panel.scope_agent, g_free);
    refresh_alerts(user_data);
}

/*
 * A row.  Errors and notices get a card and a tone; routine entries get
 * neither -- a muted line -- so the loud ones stay loud when the filter
 * widens and the routine stream becomes the majority.
 */
static LrgWidget *
alert_row(ClawtLrgApp *app, ClawtLrgAlert *alert, guint index)
{
    LrgWidget *box = clawt_lrg_vbox(4.0f);
    LrgWidget *meta = clawt_lrg_hbox(6.0f);
    g_autofree gchar *when = clawt_lrg_ago(alert->ts);

    if (alert->tier == CLAWT_ALERT_ROUTINE) {
        clawt_lrg_take(box, clawt_lrg_paragraph(alert->text, "muted"));
        lrg_container_set_padding(LRG_CONTAINER(box), 6.0f);
    } else {
        LrgWidget *head = clawt_lrg_hbox(8.0f);

        clawt_lrg_take(head, clawt_lrg_badge(
            alert->tier == CLAWT_ALERT_ERROR ? "error" : "notice",
            alert->tier == CLAWT_ALERT_ERROR ? "bad" : "info"));

        if (!alert->read)
            clawt_lrg_take(head, clawt_lrg_badge("new", "warn"));

        clawt_lrg_take(box, head);
        clawt_lrg_take(box, clawt_lrg_paragraph(alert->text, NULL));
        lrg_widget_add_class(box, "clawt-card");
        lrg_container_set_padding(LRG_CONTAINER(box), 12.0f);
    }

    if (alert->agent != NULL && *alert->agent != '\0') {
        LrgWidget *who = clawt_lrg_button(alert->agent, "flat",
                                          G_CALLBACK(on_scope_clicked), app);

        g_object_set_data_full(G_OBJECT(who), "agent",
                               g_strdup(alert->agent), g_free);
        clawt_lrg_take(meta, who);
    }

    clawt_lrg_take(meta, clawt_lrg_label(when, "caption"));
    clawt_lrg_take(box, meta);
    clawt_lrg_appear(app, box, index);

    return box;
}

static void
on_history(ClawtLrgApp *app, JsonNode *reply, const GError *error,
           gpointer user_data)
{
    JsonObject *root = clawt_ipc_reply_object(reply);
    JsonArray *events;
    guint i;

    if (alerts_panel.history == NULL)
        return;

    clawt_lrg_clear(alerts_panel.history);

    if (root == NULL || !json_object_has_member(root, "events"))
        return;

    events = json_object_get_array_member(root, "events");

    /* newest first, like the live list above it */
    for (i = json_array_get_length(events); i > 0; i--) {
        JsonObject *one = json_array_get_object_element(events, i - 1);
        const gchar *kind = clawt_ipc_payload_string(one, "kind");
        const gchar *subject = clawt_ipc_payload_string(one, "subject");
        g_autofree gchar *text = g_strdup_printf(
            "%s · %s", (kind != NULL) ? kind : "?",
            (subject != NULL) ? subject : "the fleet");

        clawt_lrg_take(alerts_panel.history, clawt_lrg_label(text, "caption"));
    }
}

static void
on_history_toggled(LrgExpander *expander, gboolean expanded,
                   gpointer user_data)
{
    ClawtLrgApp *app = user_data;

    if (!expanded)
        return;

    /* a round trip, so only when somebody asks for it */
    clawt_lrg_request(app, "event.list",
                      clawt_ipc_payload_new("limit", "50",
                                            alerts_panel.scope_agent != NULL
                                                ? "subject" : NULL,
                                            alerts_panel.scope_agent, NULL),
                      CLAWT_LRG_QUIET, on_history, NULL, NULL);
}

static void
refresh_alerts(ClawtLrgApp *app)
{
    guint shown = 0;
    guint i;

    if (alerts_panel.list == NULL)
        return;

    clawt_lrg_clear(alerts_panel.list);

    if (alerts_panel.scope_agent != NULL) {
        g_autofree gchar *label =
            g_strdup_printf("Only %s  ×", alerts_panel.scope_agent);

        clawt_lrg_take(alerts_panel.list,
                       clawt_lrg_button(label, "flat",
                                        G_CALLBACK(on_scope_cleared), app));
    }

    for (i = 0; alerts_panel.alerts != NULL && i < alerts_panel.alerts->len;
         i++) {
        ClawtLrgAlert *alert = g_ptr_array_index(alerts_panel.alerts, i);

        if (!clawt_lrg_alert_visible(alert, alerts_panel.alerts_show_all,
                                     alerts_panel.scope_agent))
            continue;

        clawt_lrg_take(alerts_panel.list, alert_row(app, alert, shown));
        shown++;
    }

    if (shown == 0)
        clawt_lrg_take(alerts_panel.list, clawt_lrg_empty_state(
            alerts_panel.alerts_show_all ? "Nothing has happened."
                                         : "Nothing has needed you.",
            NULL));

    /* the durable copy, from the daemon's own event log, on request */
    {
        LrgExpander *expander =
            lrg_expander_new("Earlier, from the daemon's log");

        alerts_panel.history = clawt_lrg_vbox(2.0f);
        lrg_expander_set_child(expander, alerts_panel.history);
        g_object_unref(alerts_panel.history);
        g_signal_connect(expander, "toggled", G_CALLBACK(on_history_toggled),
                         app);
        clawt_lrg_take(alerts_panel.list, LRG_WIDGET(expander));
    }

    clawt_lrg_app_wake(app);
}

static void
on_show_all_toggled(LrgCheckbox *toggle, gboolean checked, gpointer user_data)
{
    alerts_panel.alerts_show_all = checked;
    refresh_alerts(user_data);
}

static void
on_close_clicked(LrgButton *button, gpointer user_data)
{
    clawt_lrg_alerts_toggle(user_data);
}

static void
on_bell_clicked(LrgButton *button, gpointer user_data)
{
    clawt_lrg_alerts_toggle(user_data);
}

LrgWidget *
clawt_lrg_alerts_bell(ClawtLrgApp *app)
{
    LrgWidget *bell = clawt_lrg_button("Alerts", "flat",
                                       G_CALLBACK(on_bell_clicked), app);

    alerts_panel.bell = bell;
    lrg_widget_set_valign(bell, LRG_ALIGN_CENTER);
    return bell;
}

LrgWidget *
build_alerts_panel(ClawtLrgApp *app)
{
    LrgWidget *drawer = clawt_lrg_vbox(10.0f);
    LrgWidget *head = clawt_lrg_hbox(8.0f);
    LrgWidget *title = clawt_lrg_label("Alerts", "heading");
    LrgScrollBox *scroll = lrg_scroll_box_new();

    if (alerts_panel.alerts == NULL)
        alerts_panel.alerts = clawt_lrg_alerts_new();

    lrg_widget_add_class(drawer, "clawt-chrome");
    lrg_container_set_padding(LRG_CONTAINER(drawer), 14.0f);
    lrg_widget_set_min_size(drawer, 340.0f * (gfloat)app->scale, 0.0f);
    lrg_widget_set_max_size(drawer, 340.0f * (gfloat)app->scale, -1.0f);

    lrg_widget_set_fill_weight(title, 1.0f);
    clawt_lrg_take(head, title);
    clawt_lrg_take(head, clawt_lrg_button("Close", "flat",
                                          G_CALLBACK(on_close_clicked), app));
    clawt_lrg_take(drawer, head);

    alerts_panel.show_all_toggle =
        clawt_lrg_switch("Show everything", alerts_panel.alerts_show_all);
    g_signal_connect(alerts_panel.show_all_toggle, "toggled",
                     G_CALLBACK(on_show_all_toggled), app);
    clawt_lrg_take(drawer, alerts_panel.show_all_toggle);

    alerts_panel.list = clawt_lrg_vbox(8.0f);
    lrg_scroll_box_set_hscroll(scroll, FALSE);
    lrg_scroll_box_set_vscroll(scroll, TRUE);
    lrg_scroll_box_set_child(scroll, alerts_panel.list);
    g_object_unref(alerts_panel.list);
    lrg_widget_set_fill_weight(LRG_WIDGET(scroll), 1.0f);
    clawt_lrg_take(drawer, LRG_WIDGET(scroll));

    lrg_widget_set_visible(drawer, FALSE);
    lrg_widget_set_appear(drawer, LRG_APPEAR_FLY_LEFT, 0.0f);
    alerts_panel.drawer = drawer;

    refresh_alerts(app);
    return drawer;
}

void
clawt_lrg_alerts_toggle(ClawtLrgApp *app)
{
    if (alerts_panel.drawer == NULL)
        return;

    alerts_panel.showing = !alerts_panel.showing;
    lrg_widget_set_visible(alerts_panel.drawer, alerts_panel.showing);

    /* opening it is seeing them */
    if (alerts_panel.showing) {
        clawt_lrg_alerts_mark_read(alerts_panel.alerts);
        lrg_widget_play_appear(alerts_panel.drawer);
        refresh_alerts(app);
    }

    update_bell(app);
}

void
clawt_lrg_alerts_event(ClawtLrgApp *app, ClawtEvent *event)
{
    if (alerts_panel.alerts == NULL)
        alerts_panel.alerts = clawt_lrg_alerts_new();

    if (!clawt_lrg_alerts_add_event(alerts_panel.alerts, event,
                                    alerts_panel.showing))
        return;

    /* rebuilt only when somebody can see it: every event lands here */
    if (alerts_panel.showing)
        refresh_alerts(app);

    update_bell(app);
}

void
clawt_lrg_alerts_reset(void)
{
    g_clear_pointer(&alerts_panel.alerts, g_ptr_array_unref);
    g_clear_pointer(&alerts_panel.scope_agent, g_free);
    alerts_panel.drawer = NULL;
    alerts_panel.list = NULL;
    alerts_panel.bell = NULL;
    alerts_panel.history = NULL;
    alerts_panel.showing = FALSE;
}
