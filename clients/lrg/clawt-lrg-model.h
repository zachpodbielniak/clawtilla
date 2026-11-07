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
 * markdown label fed this.  Newlines are kept as hard breaks, because a
 * person's line break is part of what they wrote.
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

/* ── Alerts (lrg-alerts-model.c) ──────────────────────────────────── */

/* How many alerts are kept, matching GTK's panel and `agent.logs`. */
#define CLAWT_LRG_ALERTS_KEPT (200)

/**
 * ClawtLrgAlert:
 * @tier: how loud it is (clawt_alert_tier_for_event())
 * @text: what happened, in words
 * @source: what kind of thing it came from (an event kind, "download")
 * @agent: the agent or room it concerns; "" for the fleet
 * @ts: when it arrived, microseconds
 * @read: whether it has been seen
 */
typedef struct {
    ClawtAlertTier  tier;
    gchar          *text;
    gchar          *source;
    gchar          *agent;
    gint64          ts;
    gboolean        read;
} ClawtLrgAlert;

/**
 * clawt_lrg_alerts_new:
 *
 * Returns: (transfer full) (element-type ClawtLrgAlert): an empty store,
 *   newest first
 */
GPtrArray *clawt_lrg_alerts_new(void);

/**
 * clawt_lrg_alerts_add:
 * @alerts: the store
 * @tier: the tier
 * @source: (nullable): where it came from
 * @agent: (nullable): who it concerns
 * @text: what happened
 * @now_us: the time, microseconds
 * @showing: whether the alerts panel is open, so it arrives read
 *   (clawt_alert_arrives_read())
 *
 * Adds one at the top, dropping the oldest past %CLAWT_LRG_ALERTS_KEPT.
 */
void clawt_lrg_alerts_add(GPtrArray      *alerts,
                          ClawtAlertTier  tier,
                          const gchar    *source,
                          const gchar    *agent,
                          const gchar    *text,
                          gint64          now_us,
                          gboolean        showing);

/**
 * clawt_lrg_alerts_add_event:
 * @alerts: the store
 * @event: a daemon event
 * @showing: whether the panel is open
 *
 * What GTK records for an event, worded as GTK words it: a refused
 * message and a failed download are errors, said in a sentence; anything
 * else the classifier calls routine or a notice is recorded quietly as
 * "kind · subject" ("x is state" for agent.state); the rest is skipped.
 *
 * Returns: %TRUE if an alert was added
 */
gboolean clawt_lrg_alerts_add_event(GPtrArray  *alerts,
                                    ClawtEvent *event,
                                    gboolean    showing);

/**
 * clawt_lrg_alerts_unread:
 * @alerts: the store
 *
 * Returns: how many errors and notices are unread; routine entries are
 *   never counted, or the bell would never read zero
 */
guint clawt_lrg_alerts_unread(GPtrArray *alerts);

/* Marks every alert read: the panel was opened. */
void clawt_lrg_alerts_mark_read(GPtrArray *alerts);

/**
 * clawt_lrg_alert_visible:
 * @alert: an alert
 * @show_all: whether routine entries are shown too
 * @agent: (nullable): show only this agent's, or %NULL for the fleet
 *
 * Returns: whether the panel draws @alert
 */
gboolean clawt_lrg_alert_visible(const ClawtLrgAlert *alert,
                                 gboolean             show_all,
                                 const gchar         *agent);

G_END_DECLS
