/*
 * clawt-choices.h - The fixed lists a setting's drop-down offers
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Settings stored as strings whose values are nevertheless a fixed set
 * (since 0.51.3): model effort, the weekday of a weekly routine, its
 * schedule, who reviews work, and the screen sizes worth offering.  Each
 * is a family -- a count, the nth value, the nth label -- so every
 * client draws the same drop-down in the same order with the same words,
 * and `make parity` makes all three walk it: a list copied into a client
 * is a list that drifts.
 *
 * A value outside the list -- set by hand in the YAML, or by an older
 * client -- is not the list's to refuse: the clients keep it as an extra
 * entry rather than show the first choice and save that over it.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

/* ── model.effort ────────────────────────────────────────────────── */

/**
 * clawt_effort_count:
 *
 * Returns: how many entries the effort drop-down has: "the model's
 *   default" (an empty value) and the five levels the CLIs take
 */
guint clawt_effort_count(void);

/**
 * clawt_effort_nth:
 * @n: an index below clawt_effort_count()
 *
 * Returns: (transfer none): the value written to `model.effort`: "" for
 *   the default, then low, medium, high, xhigh, max
 */
const gchar *clawt_effort_nth(guint n);

/**
 * clawt_effort_nth_label:
 * @n: an index below clawt_effort_count()
 *
 * Returns: (transfer none): how the entry reads
 */
const gchar *clawt_effort_nth_label(guint n);

/* ── routines.weekday ────────────────────────────────────────────── */

/**
 * clawt_weekday_count:
 *
 * Returns: 7
 */
guint clawt_weekday_count(void);

/**
 * clawt_weekday_nth:
 * @n: 0 for Sunday, as cron counts
 *
 * Returns: (transfer none): the value written to `routines.weekday`
 */
const gchar *clawt_weekday_nth(guint n);

/**
 * clawt_weekday_nth_label:
 * @n: 0 for Sunday
 *
 * Returns: (transfer none): the day, capitalised
 */
const gchar *clawt_weekday_nth_label(guint n);

/* ── routines.schedule ───────────────────────────────────────────── */

/**
 * clawt_schedule_count:
 *
 * Returns: how many schedules a routine can have
 */
guint clawt_schedule_count(void);

/**
 * clawt_schedule_nth:
 * @n: an index below clawt_schedule_count()
 *
 * Returns: (transfer none): the nick written to `routines.schedule`, in
 *   the enumeration's order
 */
const gchar *clawt_schedule_nth(guint n);

/**
 * clawt_schedule_nth_label:
 * @n: an index below clawt_schedule_count()
 *
 * Returns: (transfer none): how the entry reads
 */
const gchar *clawt_schedule_nth_label(guint n);

/* ── review_by (agents and projects) ─────────────────────────────── */

/**
 * clawt_review_by_count:
 *
 * The fixed half of a reviewer drop-down; every agent id follows it,
 * from whatever agent list the client already has.
 *
 * Returns: how many fixed entries there are
 */
guint clawt_review_by_count(void);

/**
 * clawt_review_by_nth:
 * @n: an index below clawt_review_by_count()
 *
 * Returns: (transfer none): "" (not set), then none, operator,
 *   delegator, lead
 */
const gchar *clawt_review_by_nth(guint n);

/**
 * clawt_review_by_nth_label:
 * @n: an index below clawt_review_by_count()
 * @project: %TRUE for a project's reviewer, whose "not set" falls back
 *   to the agent's own rather than to no review
 *
 * Returns: (transfer none): how the entry reads
 */
const gchar *clawt_review_by_nth_label(guint n, gboolean project);

/**
 * clawt_review_by_agent_label:
 * @agent_id: an agent
 *
 * Returns: (transfer full): how an agent reads in a reviewer drop-down
 */
gchar *clawt_review_by_agent_label(const gchar *agent_id);

/* ── computer.vm.resolution and computer.display.resolution ──────── */

/**
 * clawt_resolution_count:
 *
 * Returns: how many screen sizes are offered
 */
guint clawt_resolution_count(void);

/**
 * clawt_resolution_nth:
 * @n: an index below clawt_resolution_count()
 *
 * Returns: (transfer none): a size, WIDTHxHEIGHT, narrowest first
 */
const gchar *clawt_resolution_nth(guint n);

G_END_DECLS
