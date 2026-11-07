/*
 * lrg-island-model.c - What the island at the top of the window says
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The island is the one line a person reads without meaning to: it sits
 * above every page, so whatever it says is what they believe about the
 * fleet.  Hence one rule, here, for what it says first.
 */

#include "clawt-lrg-model.h"

gchar *
clawt_lrg_island_text(const ClawtLrgIslandState *state)
{
    GString *out;

    g_return_val_if_fail(state != NULL, NULL);

    /*
     * Most urgent first.  A disconnected window's counts are stale, so
     * they are not shown at all -- "3 turns running" from a daemon that
     * stopped answering a minute ago is the confident wrong answer.
     */
    if (!state->connected)
        return g_strdup("Not connected");

    out = g_string_new(NULL);

    if (state->held)
        g_string_append(out, "Fleet paused");

    if (state->decisions > 0) {
        if (out->len > 0)
            g_string_append(out, " · ");

        g_string_append_printf(out,
                               g_dngettext(NULL, "%u decision waiting",
                                           "%u decisions waiting",
                                           state->decisions),
                               state->decisions);
    }

    if (state->turns > 0) {
        if (out->len > 0)
            g_string_append(out, " · ");

        if (state->agents > 0 && state->agents != state->turns)
            g_string_append_printf(out, "%u turns across %u agents",
                                   state->turns, state->agents);
        else
            g_string_append_printf(out,
                                   g_dngettext(NULL, "%u turn running",
                                               "%u turns running",
                                               state->turns),
                                   state->turns);
    }

    if (state->unread > 0) {
        if (out->len > 0)
            g_string_append(out, " · ");

        g_string_append_printf(out, "%u unread", state->unread);
    }

    if (out->len == 0)
        g_string_append(out, "Fleet idle");

    /*
     * Spend last and only when known: -1 is unknown, never free
     * (AGENTS.md, "Cost comes from the provider").
     */
    if (state->spend_micros >= 0) {
        g_autofree gchar *cost = clawt_usage_format_cost(state->spend_micros);

        g_string_append_printf(out, " · %s today", cost);
    }

    return g_string_free(out, FALSE);
}

gboolean
clawt_lrg_island_alert(const ClawtLrgIslandState *state)
{
    g_return_val_if_fail(state != NULL, FALSE);

    return !state->connected || state->decisions > 0;
}

void
clawt_lrg_island_fold_activity(ClawtLrgIslandState *state, JsonNode *reply)
{
    JsonObject *object;
    JsonArray *turns;
    g_autoptr(GHashTable) agents = NULL;
    guint length;
    guint i;

    g_return_if_fail(state != NULL);

    state->turns = 0;
    state->agents = 0;

    object = clawt_ipc_reply_object(reply);

    if (object == NULL || !json_object_has_member(object, "turns"))
        return;

    turns = json_object_get_array_member(object, "turns");
    length = (turns != NULL) ? json_array_get_length(turns) : 0;

    /*
     * Distinct agents counted by id, not by summing `running_turns`:
     * that member repeats the agent's total on each of its rows, so
     * summing it counts an agent in three rooms nine times.
     */
    agents = g_hash_table_new(g_str_hash, g_str_equal);

    for (i = 0; i < length; i++) {
        JsonObject *turn = json_array_get_object_element(turns, i);
        const gchar *agent = clawt_ipc_payload_string(turn, "agent");

        state->turns++;

        if (agent != NULL)
            g_hash_table_add(agents, (gpointer)agent);
    }

    state->agents = g_hash_table_size(agents);
}
