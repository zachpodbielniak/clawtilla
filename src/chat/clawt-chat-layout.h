/*
 * clawt-chat-layout.h - The chat column's geometry, decided once
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

/**
 * clawt_chat_body_inset:
 * @row_margin: how far a row is inset from the clamp
 * @gutter: the avatar column a body is indented past
 *
 * Where a message body starts, measured from the clamp's edge.
 *
 * Trivial arithmetic, and it is here rather than spelled out in each
 * client because the composer is the one thing on the page that has to
 * agree with a number it does not draw.  Both clients had the same
 * fault: the transcript spent a gutter on the avatar, the composer
 * spent nothing, and the strongest vertical line on the page therefore
 * stood inside the one column deliberately kept empty.
 *
 * The two arguments differ per client -- GTK insets 12 and gutters 44,
 * the web sheet gutters 36 with no inset -- so what is shared is the
 * derivation and not the numbers.  A client passing its own pair gets
 * the one answer its transcript and its composer must both use.
 *
 * Returns: the leading inset a body, and therefore the composer, takes
 */
gint clawt_chat_body_inset(gint row_margin, gint gutter);

/**
 * CLAWT_CHAT_CLAMP_WIDTH:
 *
 * The transcript's clamp, which is libadwaita's own default and is left
 * at it -- a default is a number the platform may revise, a hardcoded
 * one is a number somebody has to maintain.  Named here because the
 * alerts threshold is derived from it and a derivation needs its inputs
 * spelled rather than recalled.
 */
#define CLAWT_CHAT_CLAMP_WIDTH 600

/**
 * CLAWT_ALERTS_SIDEBAR_WIDTH:
 *
 * What the agent list takes at any window wide enough for this to
 * matter.  It is libadwaita's default `max-sidebar-width`, so the list
 * is at it well before the alerts threshold is in question.
 */
#define CLAWT_ALERTS_SIDEBAR_WIDTH 280

/**
 * CLAWT_ALERTS_PANEL_FRACTION:
 *
 * The alerts panel's share of the content beside the agent list.
 *
 * `AdwOverlaySplitView` has no sidebar-width property -- it takes a
 * fraction clamped by min and max, so this is the number that decides
 * the panel's width and therefore the threshold below it.
 */
#define CLAWT_ALERTS_PANEL_FRACTION 0.26

/**
 * CLAWT_ALERTS_PUSH_BREAKPOINT:
 *
 * The width the client actually breaks at, in pixels.
 *
 * A round number at or above clawt_alerts_push_min_width(), not the
 * floor itself: the floor is where the layout stops being wrong, which
 * is a poor place to sit.  The test asserts the relationship rather
 * than the value, so changing the fraction or the sidebar width fails
 * the suite instead of quietly leaving the column against the panel.
 */
#define CLAWT_ALERTS_PUSH_BREAKPOINT 1150

/**
 * CLAWT_ALERTS_PANEL_GAP:
 *
 * The clear space wanted between the alerts panel and a row's ink.
 *
 * The panel's edge is a hard vertical line and a row's text runs up to
 * it, so this is measured to the *ink* rather than to the column: the
 * row margin counts towards it, which is why the threshold below is not
 * simply the clamp plus the panel.
 */
#define CLAWT_ALERTS_PANEL_GAP 24

/**
 * clawt_alerts_push_min_width:
 * @sidebar_width: the agent list's width
 * @panel_fraction: the panel's share of what is left, 0 to 1
 * @clamp_width: the transcript's clamp
 * @row_margin: a row's inset inside that clamp
 *
 * The narrowest window at which the alerts panel can push the
 * transcript aside rather than overlay it.
 *
 * Derived rather than chosen, because every input is a number some
 * other line of code owns: change the panel's fraction or the sidebar's
 * width and a hardcoded breakpoint goes quietly wrong -- the panel
 * still opens, the transcript still renders, and the column simply sits
 * against the panel's edge with nowhere to breathe.  Nothing warns,
 * because nothing is broken.
 *
 * Fitting the clamp is deliberately *not* the criterion.  That is met a
 * long way below this and looks wrong when it is met, the column
 * finishing a few pixels from the panel.  The criterion is
 * %CLAWT_ALERTS_PANEL_GAP of clear space to the ink, which the row
 * margin contributes to.
 *
 * Returns: the minimum window width, in pixels, or 0 if @panel_fraction
 *   leaves the transcript nothing (which no layout can satisfy)
 */
gint clawt_alerts_push_min_width(gint    sidebar_width,
                                 gdouble panel_fraction,
                                 gint    clamp_width,
                                 gint    row_margin);

G_END_DECLS
