/*
 * gtk-hold.c - Holding the fleet from the graphical client
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawt-window-private.h"

/*
 * Warned first, because a hold is not a stop and the difference is the
 * only thing somebody needs to know before pressing it.
 *
 * A stop would kill every turn in flight; this lets them finish and
 * stops the next one starting.  Said in the dialog rather than in a
 * tooltip, because the word "pause" carries the wrong promise in most
 * software -- and an operator who thinks it kills work will not use it,
 * which is the state this feature exists to end.
 */
void
clawt_gtk_on_pause_fleet(GSimpleAction *action, GVariant *parameter,
                         gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(JsonNode) reply = NULL;
    JsonObject *hold;
    gint64 draining;

    (void)action;
    (void)parameter;

    reply = clawt_window_request(self, "control.pause", NULL);

    if (reply == NULL)
        return;

    hold = json_object_get_object_member(clawt_payload_of(reply), "hold");
    draining = (hold != NULL)
        ? json_object_get_int_member(hold, "draining") : 0;

    /*
     * A toast, because this answers a question somebody is holding
     * right now.  What the fleet is *in* until they resume is the
     * banner's job, and it draws itself from the next status read.
     */
    if (draining > 0) {
        g_autofree gchar *text = g_strdup_printf(
            "Held. %" G_GINT64_FORMAT " turn(s) still finishing.",
            draining);

        clawt_window_toast(self, text);
    } else {
        clawt_window_toast(self, "Held, with nothing in flight.");
    }

    clawt_gtk_refresh_agents(self);
}

void
clawt_gtk_on_resume_fleet(GSimpleAction *action, GVariant *parameter,
                          gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(JsonNode) reply = NULL;

    (void)action;
    (void)parameter;

    reply = clawt_window_request(self, "control.resume", NULL);

    if (reply == NULL)
        return;

    clawt_window_toast(self,
                       "Resumed. Queued work goes out now, in order.");
    clawt_gtk_refresh_agents(self);
}
