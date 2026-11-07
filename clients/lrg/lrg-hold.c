/*
 * lrg-hold.c - Pausing and resuming the fleet
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Mirrors clients/gtk/gtk-hold.c, plus the `control.status` read the GTK
 * window does at connect (note_daemon_version() in clawt-window.c),
 * and the daemon's version and any update it found, which the shell's
 * banner draws.
 *
 * The shell owns the banner and the island; this file owns the one bit
 * they draw from -- app->held -- and keeps it true to the daemon.  It is
 * read from `control.status`, which reports the hold always ("not held"
 * included), so a client never has to infer it from an absent member.
 */

#include "clawt-lrg.h"
#include "clawt-lrg-agent-model.h"

static void
apply_hold(ClawtLrgApp *app, JsonNode *reply)
{
    app->held = clawt_lrg_hold_is_held(reply);
    clawt_lrg_update_banner(app);
    clawt_lrg_update_island(app);
}

/*
 * What control.status says about the daemon itself: its version, and an
 * update it found.  The shell's banner draws the sentence from these
 * (clawt_lrg_update_banner()), as GTK's does -- a condition the window
 * is in, not an answer to something somebody just did, so not a toast.
 */
static void
note_daemon_version(ClawtLrgApp *app, JsonNode *reply)
{
    g_free(app->daemon_version);
    app->daemon_version = g_strdup(clawt_ipc_payload_string(
        clawt_ipc_reply_object(reply), "version"));

    g_free(app->daemon_update);
    app->daemon_update = clawt_lrg_hold_available_update(reply);

    clawt_lrg_update_banner(app);
}

static void
on_status(ClawtLrgApp *app, JsonNode *reply, const GError *error,
          gpointer user_data)
{
    /*
     * A failed read leaves the last answer standing: "not connected" is
     * the banner's to say, and dropping a hold the daemon still has
     * because one request failed would draw the fleet as running.
     */
    if (reply == NULL)
        return;

    apply_hold(app, reply);
    note_daemon_version(app, reply);
}

void
clawt_lrg_hold_refresh(ClawtLrgApp *app)
{
    if (!clawt_client_is_connected(app->client))
        return;

    clawt_lrg_request(app, "control.status", NULL, CLAWT_LRG_QUIET,
                      on_status, NULL, NULL);
}

static void
on_paused(ClawtLrgApp *app, JsonNode *reply, const GError *error,
          gpointer user_data)
{
    g_autofree gchar *said = NULL;

    if (reply == NULL)
        return;

    /*
     * A toast, because it answers what somebody just did; what the fleet
     * is *in* until it is resumed is the banner's, drawn from app->held.
     */
    said = clawt_lrg_hold_paused_text(reply);
    clawt_lrg_toast(app, said);
    apply_hold(app, reply);
    clawt_lrg_refresh_fleet(app);
}

static void
on_pause_confirmed(ClawtLrgApp *app, gpointer user_data)
{
    clawt_lrg_request(app, "control.pause", NULL, CLAWT_LRG_REQUEST_NONE,
                      on_paused, NULL, NULL);
}

void
clawt_lrg_hold_pause(ClawtLrgApp *app)
{
    /*
     * Asked first, because a hold is not a stop and the difference is
     * the only thing somebody needs to know before pressing it.  "Pause"
     * carries the wrong promise in most software, and an operator who
     * thinks it kills work will not use it -- which is the state this
     * feature exists to end.
     */
    clawt_lrg_confirm(
        app, "Hold the fleet?",
        "Nothing new is delivered and no routine runs until you resume. "
        "Turns already running are left to finish -- nothing in flight is "
        "killed -- and everything that arrives meanwhile waits in order.",
        "Hold", FALSE, on_pause_confirmed, NULL, NULL);
}

static void
on_resumed(ClawtLrgApp *app, JsonNode *reply, const GError *error,
           gpointer user_data)
{
    if (reply == NULL)
        return;

    clawt_lrg_toast(app, "Resumed. Queued work goes out now, in order.");
    apply_hold(app, reply);
    clawt_lrg_refresh_fleet(app);
}

void
clawt_lrg_hold_resume(ClawtLrgApp *app)
{
    clawt_lrg_request(app, "control.resume", NULL, CLAWT_LRG_REQUEST_NONE,
                      on_resumed, NULL, NULL);
}
