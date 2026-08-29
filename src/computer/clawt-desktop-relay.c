/*
 * clawt-desktop-relay.c - stdio MCP, from the agent's CLI into its VM
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The mechanism lives in clawt-mcp-relay.c, because it turned out not to
 * be about desktops at all: filtering the JSON-RPC going past is exactly
 * what a connector needs too.  What remains here is the desktop's own
 * name for it.
 *
 * These stay as their own functions rather than being replaced at every
 * call site.  A guest desktop reached over ssh and a tool server started
 * with a credential are different enough that somebody reading either
 * one should not have to work out which of them a shared symbol was
 * serving -- and the desktop relay's tests are written against these
 * names.
 */

#include "clawtilla.h"
#include "computer/clawt-desktop-relay.h"

#include "mcp/clawt-mcp-relay.h"

/*
 * Said in the desktop's own terms.  The relay serves connectors too, and
 * a shared message would tell an agent refused a repository tool to turn
 * on a setting about the screen.
 */
/*
 * All three grants are named, in one string, because the relay's run
 * loop carries one hint for every refusal it will ever emit -- there is
 * no per-line hook on that path, and a hint chosen per tool would exist
 * only on the filter path the tests call.
 *
 * Recording is named separately from input rather than lumped in with
 * it: an agent refused a keylogger that was told to turn on
 * `allow_input` would be given an instruction that does not work, and
 * an operator following it would hand over their pointer for nothing.
 */
#define DESKTOP_REFUSAL_HINT \
    "Seeing the screen, acting on it, launching programs and recording " \
    "what a person does are four separate grants: turn on " \
    "computer.desktop.allow_input, allow_spawn or allow_recording -- " \
    "whichever this call needed -- if this agent should have it."

gboolean
clawt_desktop_relay_filter_outbound(const gchar  *line,
                                    GStrv         permitted,
                                    gchar       **refusal)
{
    /*
     * The desktop's own reason for refusing, which is not the relay's
     * to know: seeing a screen and acting on it are separate grants
     * here, and an agent that hits this needs to be told which one it
     * is missing rather than that something was denied.
     */
    return clawt_mcp_relay_filter_outbound(
        line, permitted,
        DESKTOP_REFUSAL_HINT, refusal);
}

gchar *
clawt_desktop_relay_filter_inbound(const gchar *line, GStrv permitted)
{
    return clawt_mcp_relay_filter_inbound(line, permitted);
}

gint
clawt_desktop_relay_run_gated(GStrv              argv,
                              GStrv              permitted,
                              ClawtMcpRelayGate  gate,
                              gpointer           gate_data)
{
    return clawt_mcp_relay_run_gated(argv, NULL, permitted,
                                     DESKTOP_REFUSAL_HINT, gate, gate_data);
}

gint
clawt_desktop_relay_run(GStrv argv, GStrv permitted)
{
    /*
     * No environment.  The guest's server is reached over ssh, which
     * would not carry one across anyway, and there is no credential on
     * this side to give it -- the key is ssh's business.
     */
    return clawt_mcp_relay_run(argv, NULL, permitted, DESKTOP_REFUSAL_HINT);
}
