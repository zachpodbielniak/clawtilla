/*
 * clawt-provider-usage.h - What each provider login has left of its plan
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * ai-glib reports a CLI login's allowance windows (a five-hour session,
 * a weekly cap, a billing period) as schema-versioned JSON whose values
 * are null when the source did not say.  This file turns one of those
 * reports into the windows the daemon sends in `usage.providers`, and
 * gives every client the same words, numbers and colour for them -- so
 * "62% left" in the GTK client, the web client, the libregnum client and
 * the CLI is one decision rather than four.
 *
 * Direction matters and is easy to get backwards: Codex reports how much
 * is *used*, a native panel may report how much is *left*, and some
 * panels print a percentage with no direction at all.  Everything here
 * is phrased as remaining, converted only when the source said which way
 * it pointed; a percentage of unknown direction is shown as it was given
 * and never coloured, because a guessed direction is a meter that reads
 * "plenty" at the moment the limit is hit.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * ClawtAllowanceLevel:
 * @CLAWT_ALLOWANCE_UNKNOWN: the source gave nothing a direction can be
 *   read from; never coloured as if it were fine
 * @CLAWT_ALLOWANCE_PLENTY: more than a quarter of the window is left
 * @CLAWT_ALLOWANCE_LOW: a quarter or less is left
 * @CLAWT_ALLOWANCE_CRITICAL: a tenth or less is left
 *
 * How worried to be about one allowance window.
 */
typedef enum {
    CLAWT_ALLOWANCE_UNKNOWN,
    CLAWT_ALLOWANCE_PLENTY,
    CLAWT_ALLOWANCE_LOW,
    CLAWT_ALLOWANCE_CRITICAL
} ClawtAllowanceLevel;

/**
 * CLAWT_ALLOWANCE_LOW_PERCENT:
 *
 * At or below this much remaining, a window is %CLAWT_ALLOWANCE_LOW.
 */
#define CLAWT_ALLOWANCE_LOW_PERCENT (25)

/**
 * CLAWT_ALLOWANCE_CRITICAL_PERCENT:
 *
 * At or below this much remaining, a window is %CLAWT_ALLOWANCE_CRITICAL.
 */
#define CLAWT_ALLOWANCE_CRITICAL_PERCENT (10)

/**
 * clawt_provider_usage_normalize:
 * @report: an ai-glib usage report's data (ai_cli_report_dup_data())
 *
 * Turns an ai-glib `AI_CLI_REPORT_USAGE` document (schema version 1)
 * into the object the daemon sends for one login: `plan`, `source`,
 * `partial`, `collected_at` (Unix seconds) and `windows`, each window
 * carrying `label`, `unit`, `remaining_percent` when the source gave a
 * direction, `percent` when it gave a number with no direction, the
 * raw `used`/`limit`/`remaining` in the window's unit, `window_minutes`,
 * `reset_at` (Unix seconds, only when the source gave an exact time) and
 * `reset_text` (the source's own words, or a date it gave without a
 * time).  Unknown stays absent; it is never written as zero.
 *
 * Returns: (transfer full) (nullable): the login object, or %NULL when
 *   @report is not a schema-1 usage document
 */
JsonNode *clawt_provider_usage_normalize(JsonNode *report);

/**
 * clawt_usage_window_remaining:
 * @window: one window from `usage.providers`
 * @percent: (out) (optional): the percentage left, 0 to 100
 *
 * Returns: %TRUE when the window says how much is left
 */
gboolean clawt_usage_window_remaining(JsonObject *window, gdouble *percent);

/**
 * clawt_usage_window_level:
 * @window: one window from `usage.providers`
 *
 * Returns: how worried to be about @window
 */
ClawtAllowanceLevel clawt_usage_window_level(JsonObject *window);

/**
 * clawt_allowance_level_tone:
 * @level: a level
 *
 * Returns: (transfer none): the badge tone the clients paint @level in
 *   (`good`, `warn`, `bad` or `neutral`)
 */
const gchar *clawt_allowance_level_tone(ClawtAllowanceLevel level);

/**
 * clawt_usage_window_value_label:
 * @window: one window from `usage.providers`
 *
 * The headline figure: "62% left", "38% (direction not reported)",
 * "$3.20 of $10.00 left", "1,200 tokens used".
 *
 * Returns: (transfer full): the label
 */
gchar *clawt_usage_window_value_label(JsonObject *window);

/**
 * clawt_usage_window_reset_label:
 * @window: one window from `usage.providers`
 * @now: the current Unix time, in seconds
 *
 * When the window renews: "resets in 2h 41m" within a day, "resets Tue
 * 14:00" within a week, "resets 3 Dec" beyond that, or the source's own
 * words when it gave no exact time.  Computed from @now by the caller
 * rather than sent pre-rendered: a page left open would otherwise say
 * "in 2h" for ever.
 *
 * Returns: (transfer full) (nullable): the label, or %NULL when the
 *   source said nothing about renewal
 */
gchar *clawt_usage_window_reset_label(JsonObject *window, gint64 now);

/**
 * clawt_usage_meter_text:
 * @remaining_percent: the percentage left, 0 to 100
 * @width: the bar's width in cells
 *
 * A text bar for the CLI and anywhere a drawn meter is not available,
 * filled for what is *left*, so a full bar is good news everywhere.
 *
 * Returns: (transfer full): the bar
 */
gchar *clawt_usage_meter_text(gdouble remaining_percent, guint width);

/**
 * clawt_usage_login_title:
 * @login: one login from `usage.providers`
 *
 * "Claude Code -- work (max)": the CLI, the account when it is not the
 * default login, the plan when the provider named one.
 *
 * Returns: (transfer full): the title
 */
gchar *clawt_usage_login_title(JsonObject *login);

/**
 * clawt_usage_login_status_label:
 * @login: one login from `usage.providers`
 * @now: the current Unix time, in seconds
 *
 * The line under the title: when it was read, that it is being read, or
 * why it could not be -- "this login reports no plan limits" is a
 * different answer from a CLI that failed, and the two must not read
 * alike.
 *
 * Returns: (transfer full): the label
 */
gchar *clawt_usage_login_status_label(JsonObject *login, gint64 now);

/**
 * clawt_usage_login_level:
 * @login: one login from `usage.providers`
 *
 * Returns: the most worrying level among @login's windows
 */
ClawtAllowanceLevel clawt_usage_login_level(JsonObject *login);

G_END_DECLS
