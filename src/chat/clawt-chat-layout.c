/*
 * clawt-chat-layout.c - The chat column's geometry, decided once
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include <math.h>

#include "chat/clawt-chat-layout.h"

gint
clawt_chat_body_inset(
    gint    row_margin,
    gint    gutter
){
    return row_margin + gutter;
}

gint
clawt_alerts_push_min_width(
    gint     sidebar_width,
    gdouble  panel_fraction,
    gint     clamp_width,
    gint     row_margin
){
    gdouble transcript;
    gdouble window;

    /*
     * A fraction outside (0, 1) describes a panel that takes all of the
     * content or none of it.  Neither is a layout this threshold can be
     * derived for, so it refuses rather than returning a number that
     * looks usable.
     */
    if (panel_fraction <= 0.0 || panel_fraction >= 1.0)
        return 0;

    /*
     * The transcript keeps whatever share the panel does not, and the
     * gap is measured from the panel's edge to the row's ink -- so the
     * row margin counts towards it and the column itself only needs the
     * remainder.
     */
    transcript = (gdouble)clamp_width +
                 2.0 * (gdouble)(CLAWT_ALERTS_PANEL_GAP - row_margin);

    if (transcript < (gdouble)clamp_width)
        transcript = (gdouble)clamp_width;

    /*
     * That share is (1 - fraction) of the content beside the agent
     * list, so the window is the list plus what the share implies.
     */
    window = (gdouble)sidebar_width + transcript / (1.0 - panel_fraction);

    return (gint)ceil(window);
}
