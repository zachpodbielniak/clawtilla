/*
 * clawt-lrg-model.h - The libregnum client's decisions, without libregnum
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Everything clawtilla-lrg decides -- which rows a reply becomes, what a
 * badge counts, which theme document an appearance is, what a slash
 * command means -- lives behind this header, in clients/lrg/ *-model.c
 * files that include no libregnum header at all.  The views only draw
 * what these return.
 *
 * Two reasons.  tests/test-lrg-models.c links these files into every
 * `make test`, so the logic is checked on a machine with no libregnum
 * and no display; and a rule written here once is a rule the drawing
 * code cannot quietly re-decide in a second place.  Rules every client
 * shares belong one level further down still, in libclawt.
 */

#pragma once

#include <clawtilla.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* ── Theme (lrg-theme-model.c) ────────────────────────────────────── */

/**
 * ClawtLrgPaletteUse:
 * @adw_name: a libadwaita colour name from the shared palette vocabulary
 * @role: (nullable): the #LrgThemeColor nick it feeds, or %NULL
 * @note: what it is used for, or why it is not
 *
 * One row of the table that says what clawtilla-lrg does with each
 * colour a palette defines.  Every name clawt_appearance_palette_missing()
 * requires has a row, and tests/test-lrg-models.c fails on one that has
 * not -- so a colour added to the vocabulary later is a decision somebody
 * makes here, not a colour this client silently never draws.
 */
typedef struct {
    const gchar *adw_name;
    const gchar *role;
    const gchar *note;
} ClawtLrgPaletteUse;

/**
 * clawt_lrg_palette_uses:
 * @n_uses: (out): how many rows
 *
 * Returns: (transfer none) (array length=n_uses): the palette table
 */
const ClawtLrgPaletteUse *clawt_lrg_palette_uses(guint *n_uses);

/**
 * clawt_lrg_stock_palette:
 * @dark: which of libadwaita's two stock schemes
 *
 * libadwaita's stock colours, written in the palette vocabulary.  The
 * "system", "light" and "dark" schemes have no palette of their own --
 * GTK takes them from libadwaita and the web client from its stylesheet
 * -- so clawtilla-lrg reads these instead, through the same reader as a
 * palette file.
 *
 * Returns: (transfer none): palette CSS defining every required colour
 */
const gchar *clawt_lrg_stock_palette(gboolean dark);

/**
 * clawt_lrg_appearance_is_dark:
 * @appearance: the appearance
 * @system_dark: whether the desktop asks for dark, for the "system" scheme
 *
 * Returns: whether the scheme @appearance names draws dark
 */
gboolean clawt_lrg_appearance_is_dark(ClawtAppearance *appearance,
                                      gboolean         system_dark);

/**
 * clawt_lrg_theme_yaml:
 * @appearance: the appearance to draw
 * @system_dark: whether the desktop asks for dark
 * @scale: interface scale; 1.0 is libregnum's own sizes
 *
 * The libregnum theme document for @appearance: colour roles read from
 * its palette (or libadwaita's stock one), the accent, the motion preset,
 * the metrics at @scale, and the style rules that give clawtilla-lrg its
 * look -- materials, glows, the animated background, badge tones.
 *
 * A document rather than a sequence of setter calls so the whole look is
 * one inspectable string, checked by tests/test-lrg-models.c with no
 * libregnum present and by tests/test-lrg-theme.c by actually loading it.
 *
 * Returns: (transfer full): YAML for lrg_theme_new_from_data()
 */
gchar *clawt_lrg_theme_yaml(ClawtAppearance *appearance,
                            gboolean         system_dark,
                            gdouble          scale);

/**
 * clawt_lrg_effective_scale:
 * @appearance: the appearance
 * @monitor_scale: what the monitor asks for (its DPI over 96)
 *
 * Returns: the interface scale to draw at: the setting when there is one,
 *   else the monitor's, clamped to the range the setting allows
 */
gdouble clawt_lrg_effective_scale(ClawtAppearance *appearance,
                                  gdouble          monitor_scale);

/**
 * clawt_lrg_tone_class:
 * @tone: (nullable): a tone word from libclawt ("good", "warn", "bad",
 *   "info", "neutral")
 *
 * Returns: (transfer none): the style class that paints it; an unknown
 *   or %NULL tone is "clawt-tone-neutral"
 */
const gchar *clawt_lrg_tone_class(const gchar *tone);

/* ── Island (lrg-island-model.c) ──────────────────────────────────── */

/**
 * ClawtLrgIslandState:
 * @turns: turns running across the fleet
 * @agents: agents with a turn running
 * @decisions: decisions waiting on a person
 * @unread: unread messages across every room
 * @spend_micros: spend today in micro-dollars, or -1 for unknown
 * @connected: whether the daemon is reachable
 * @held: whether the fleet is paused
 *
 * What the window's island summarises.
 */
typedef struct {
    guint    turns;
    guint    agents;
    guint    decisions;
    guint    unread;
    gint64   spend_micros;
    gboolean connected;
    gboolean held;
} ClawtLrgIslandState;

/**
 * clawt_lrg_island_text:
 * @state: the fleet summary
 *
 * The one line the compact island shows: the most urgent thing first --
 * disconnected, then paused, then decisions waiting, then running work,
 * then idle -- so the sentence a person glances at is the one that may
 * need them.
 *
 * Returns: (transfer full): the line
 */
gchar *clawt_lrg_island_text(const ClawtLrgIslandState *state);

/**
 * clawt_lrg_island_alert:
 * @state: the fleet summary
 *
 * Returns: whether the island should glow for attention (disconnected,
 *   or a decision waiting)
 */
gboolean clawt_lrg_island_alert(const ClawtLrgIslandState *state);

/**
 * clawt_lrg_island_fold_activity:
 * @state: the summary to update
 * @reply: (nullable): an `activity.list` reply
 *
 * Counts turns and distinct agents out of an activity listing.
 */
void clawt_lrg_island_fold_activity(ClawtLrgIslandState *state,
                                    JsonNode            *reply);

/* ── Text (lrg-text-model.c) ──────────────────────────────────────── */

/**
 * clawt_lrg_markdown_escape:
 * @text: (nullable): plain text
 *
 * @text with every character markdown gives a meaning backslash-escaped,
 * so a markdown label shows it exactly as written.  libregnum's plain
 * label draws one line; a wrapping paragraph of somebody's words is a
 * markdown label fed this.  Each line becomes a paragraph of its own,
 * because a person's line break is part of what they wrote and
 * libregnum's markdown has no hard break to keep it with.
 *
 * Returns: (transfer full): escaped text; "" for %NULL
 */
gchar *clawt_lrg_markdown_escape(const gchar *text);

/**
 * clawt_lrg_image_template:
 * @prefix: what the file is for ("clawtilla-avatar")
 * @mime: (nullable): a type clawt_avatar_sniff_mime_type() returned
 *
 * A g_file_open_tmp() template for image bytes that graylib must load:
 * graylib picks the decoder from the file's extension, so the template
 * ends in ".<ext>".  All three callers once wrote "XXXXXX%s" with the
 * bare extension -- "clawtilla-avatar-YEW1X3jpg" -- and every avatar,
 * chat picture and screen frame failed to load with "Missing file
 * extension", drawing initials and an empty Screen panel instead.
 *
 * Returns: (transfer full) (nullable): the template, or %NULL for a type
 *   with no extension (which graylib could not load either)
 */
gchar *clawt_lrg_image_template(const gchar *prefix, const gchar *mime);

/* ── Requests (lrg-text-model.c) ──────────────────────────────────── */

/**
 * clawt_lrg_reply_is_current:
 * @guarded: whether the request asked for %CLAWT_LRG_GUARDED
 * @sent: the selection generation when the request was sent
 * @now: the generation when the reply arrived
 *
 * Whether a reply may still be delivered.  An unguarded one always may;
 * a guarded one only while nobody has selected something else since --
 * a reply about the page somebody left must not repaint the page they
 * are on.  A generation counter that wrapped is "changed", never "same".
 *
 * Returns: %TRUE to deliver the reply
 */
gboolean clawt_lrg_reply_is_current(gboolean guarded, guint sent, guint now);

/* ── Notifications (lrg-alerts-model.c) ───────────────────────────── */

/*
 * The drawer is a view of the daemon's `notice.list`; nothing about an
 * alert is decided here any more -- the tier, the words and what counts
 * are the daemon's.  What is left is the two decisions the drawer and
 * the sidebar make about a reply.
 */

/**
 * clawt_lrg_notice_in_scope:
 * @agent: (nullable): the agent a row is about
 * @scope: (nullable): the agent the drawer is narrowed to, or %NULL
 *
 * Returns: whether the drawer draws the row: always when it is not
 *   narrowed, and only that agent's rows when it is
 */
gboolean clawt_lrg_notice_in_scope(const gchar *agent, const gchar *scope);

/**
 * clawt_lrg_muted_take:
 * @muted: (element-type utf8 gpointer): the app's muted set
 * @from: (element-type utf8 gpointer): the set a `notice.list` names
 *   (clawt_notice_list_mutes())
 *
 * Makes @muted hold exactly @from.
 *
 * Returns: %TRUE if it changed -- the sidebar redraws only then, since
 *   every message brings a new list
 */
gboolean clawt_lrg_muted_take(GHashTable *muted, GHashTable *from);

G_END_DECLS
