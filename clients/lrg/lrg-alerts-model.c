/*
 * lrg-alerts-model.c - What the alerts panel records, and what it counts
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The same rules as GTK's panel (clients/gtk/clawt-window.c), which the
 * library already holds the hard parts of: the tier is
 * clawt_alert_tier_for_event(), and whether an arriving alert has been
 * seen is clawt_alert_arrives_read().  What is here is the wording and
 * the bookkeeping, so the drawer in lrg-alerts.c only draws.
 */

#include "clawt-lrg-model.h"

static void
alert_free(gpointer data)
{
    ClawtLrgAlert *alert = data;

    g_free(alert->text);
    g_free(alert->source);
    g_free(alert->agent);
    g_free(alert);
}

GPtrArray *
clawt_lrg_alerts_new(void)
{
    return g_ptr_array_new_with_free_func(alert_free);
}

void
clawt_lrg_alerts_add(GPtrArray *alerts, ClawtAlertTier tier,
                     const gchar *source, const gchar *agent,
                     const gchar *text, gint64 now_us, gboolean showing)
{
    ClawtLrgAlert *alert;

    g_return_if_fail(alerts != NULL);

    if (text == NULL || tier == CLAWT_ALERT_SKIP)
        return;

    alert = g_new0(ClawtLrgAlert, 1);
    alert->tier = tier;
    alert->text = g_strdup(text);
    alert->source = g_strdup((source != NULL) ? source : "");
    alert->agent = g_strdup((agent != NULL) ? agent : "");
    alert->ts = now_us;

    /*
     * One that lands in front of somebody has been seen as it arrived;
     * otherwise the bell counts a row they watched appear.
     */
    alert->read = clawt_alert_arrives_read(showing, tier);

    /* newest first: the panel is read from the top */
    g_ptr_array_insert(alerts, 0, alert);

    while (alerts->len > CLAWT_LRG_ALERTS_KEPT)
        g_ptr_array_remove_index(alerts, alerts->len - 1);
}

gboolean
clawt_lrg_alerts_add_event(GPtrArray *alerts, ClawtEvent *event,
                           gboolean showing)
{
    const gchar *kind;
    const gchar *subject;
    ClawtAlertTier tier;
    g_autofree gchar *line = NULL;
    gint64 now = g_get_real_time();

    g_return_val_if_fail(alerts != NULL, FALSE);
    g_return_val_if_fail(event != NULL, FALSE);

    kind = clawt_event_get_kind(event);
    subject = clawt_event_get_subject(event);

    /*
     * The two loud ones are said in a sentence.  Both arrive on their
     * own -- a download outlives the window that started it, and a
     * refused message just makes two agents go quiet -- so a toast that
     * vanishes in four seconds would leave no trace anywhere.
     */
    if (g_strcmp0(kind, "message.refused") == 0) {
        const gchar *from = clawt_event_get_detail(event, "from");
        const gchar *to = clawt_event_get_detail(event, "to");
        const gchar *reason = clawt_event_get_detail(event, "reason");

        line = g_strdup_printf("%s to %s was stopped: %s",
                               (from != NULL) ? from : "?",
                               (to != NULL) ? to : "?",
                               (reason != NULL) ? reason : "a limit");
        clawt_lrg_alerts_add(alerts, CLAWT_ALERT_ERROR, "message", from, line,
                             now, showing);
        return TRUE;
    }

    if (g_strcmp0(kind, "image.finished") == 0) {
        const gchar *failure = clawt_event_get_detail(event, "error");

        if (failure == NULL)
            return FALSE;

        line = g_strdup_printf("%s could not be downloaded: %s",
                               (subject != NULL) ? subject : "?", failure);
        clawt_lrg_alerts_add(alerts, CLAWT_ALERT_ERROR, "download", subject,
                             line, now, showing);
        return TRUE;
    }

    /*
     * A routine turned off because its computer would not start: worded
     * by libclawt, as the GTK and web panels word it.
     */
    if (g_strcmp0(kind, "routine.turned_off") == 0) {
        line = clawt_routine_turned_off_alert(
            subject, (guint)clawt_event_get_detail_int(event, "failures"),
            clawt_event_get_detail(event, "error"));
        clawt_lrg_alerts_add(alerts, CLAWT_ALERT_ERROR, kind,
                             clawt_event_get_detail(event, "agent"), line,
                             now, showing);
        return TRUE;
    }

    tier = clawt_alert_tier_for_event(event);

    if (tier != CLAWT_ALERT_ROUTINE && tier != CLAWT_ALERT_NOTICE)
        return FALSE;

    if (g_strcmp0(kind, "agent.state") == 0) {
        const gchar *state = clawt_event_get_detail(event, "state");

        line = g_strdup_printf("%s is %s", (subject != NULL) ? subject : "?",
                               (state != NULL) ? state : "?");
    } else if (clawt_event_get_detail(event, "message") != NULL) {
        /*
         * An event carrying its own sentence -- agent.failover's says
         * who, on what, why and until when -- is shown as written.
         */
        line = g_strdup(clawt_event_get_detail(event, "message"));
    } else {
        line = g_strdup_printf("%s · %s", (kind != NULL) ? kind : "?",
                               (subject != NULL) ? subject : "the fleet");
    }

    clawt_lrg_alerts_add(alerts, tier, kind, subject, line, now, showing);
    return TRUE;
}

guint
clawt_lrg_alerts_unread(GPtrArray *alerts)
{
    guint count = 0;
    guint i;

    for (i = 0; alerts != NULL && i < alerts->len; i++) {
        ClawtLrgAlert *alert = g_ptr_array_index(alerts, i);

        if (!alert->read && alert->tier != CLAWT_ALERT_ROUTINE)
            count++;
    }

    return count;
}

void
clawt_lrg_alerts_mark_read(GPtrArray *alerts)
{
    guint i;

    for (i = 0; alerts != NULL && i < alerts->len; i++)
        ((ClawtLrgAlert *)g_ptr_array_index(alerts, i))->read = TRUE;
}

gboolean
clawt_lrg_alert_visible(const ClawtLrgAlert *alert, gboolean show_all,
                        const gchar *agent)
{
    g_return_val_if_fail(alert != NULL, FALSE);

    if (!show_all && alert->tier == CLAWT_ALERT_ROUTINE)
        return FALSE;

    if (agent != NULL && g_strcmp0(agent, alert->agent) != 0)
        return FALSE;

    return TRUE;
}
