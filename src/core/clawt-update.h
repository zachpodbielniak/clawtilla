/*
 * clawt-update.h - Knowing that a newer version exists
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

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define CLAWT_TYPE_UPDATE_CHECK (clawt_update_check_get_type())

G_DECLARE_FINAL_TYPE(ClawtUpdateCheck, clawt_update_check, CLAWT,
                     UPDATE_CHECK, GObject)

/**
 * clawt_update_version_compare:
 * @a: a version, with or without a leading "v"
 * @b: a version, with or without a leading "v"
 *
 * Orders two dotted version strings.
 *
 * A leading "v" is ignored, because a release tag usually has one and
 * `CLAWT_VERSION_STRING` never does -- comparing them as plain strings
 * makes every tagged release look older than the build asking about it.
 *
 * Components are compared numerically and a missing component is zero,
 * so "0.2" and "0.2.0" are the same version.  Anything after the last
 * digit of a component -- "-rc1", "+build" -- is compared as text and
 * only when the numbers are equal, so 0.3.0 beats 0.3.0-rc1 and neither
 * beats 0.4.0.
 *
 * Returns: -1 when @a is older, 0 when they are the same, 1 when @a is
 *   newer.  A %NULL or unparseable version sorts as older than one that
 *   parses, and two of them are equal -- so a source that answers with
 *   nothing useful can never produce "an update is available"
 */
gint clawt_update_version_compare(const gchar *a, const gchar *b);

/**
 * clawt_update_version_from_json:
 * @root: (nullable): a parsed reply
 *
 * Pulls a version out of whatever an update source answered with.
 *
 * Three shapes, because three are real and none of them is ours: a bare
 * JSON string (a file somebody publishes), an object carrying `version`,
 * `tag_name` or `name` (one release), and an array of such objects
 * (Forgejo, Gitea and GitLab all answer their releases endpoint this
 * way, newest first).  Anything else is not an error here -- it is a
 * source that said nothing this code understands, which the caller
 * reports as a failed check rather than as being up to date.
 *
 * Returns: (transfer full) (nullable): the version, or %NULL
 */
gchar *clawt_update_version_from_json(JsonNode *root);

/**
 * clawt_update_check_new:
 * @current: the running version, normally %CLAWT_VERSION_STRING
 * @url: where to ask
 * @interval_hours: how often, clamped to at least one hour
 *
 * Returns: (transfer full): a checker that has not asked anything yet
 */
ClawtUpdateCheck *clawt_update_check_new(const gchar *current,
                                         const gchar *url,
                                         gint         interval_hours);

/**
 * clawt_update_check_start:
 * @self: a #ClawtUpdateCheck
 *
 * Arms the timer.  Nothing leaves the machine until it fires, which is
 * deliberate: an IPC handler must not wait on the network and neither
 * may daemon start, and a check at start would make every fixture that
 * builds a daemon reach out.
 */
void clawt_update_check_start(ClawtUpdateCheck *self);

/**
 * clawt_update_check_stop:
 * @self: a #ClawtUpdateCheck
 *
 * Disarms it.  Any request already in flight is cancelled.
 */
void clawt_update_check_stop(ClawtUpdateCheck *self);

/**
 * clawt_update_check_describe:
 * @self: a #ClawtUpdateCheck
 * @builder: a #JsonBuilder positioned inside an object
 *
 * Writes what is known under an `update` member.
 *
 * Always writes `checked_at` and, when the last attempt failed, `error`.
 * An update check that has been quietly erroring for a month is worse
 * than none, because a client with nothing to draw draws nothing and
 * that reads as "up to date".
 */
void clawt_update_check_describe(ClawtUpdateCheck *self,
                                 JsonBuilder      *builder);

/**
 * clawt_update_check_get_latest:
 * @self: a #ClawtUpdateCheck
 *
 * Returns: (transfer none) (nullable): the newest version the source has
 *   reported, or %NULL when nothing has been read yet
 */
const gchar *clawt_update_check_get_latest(ClawtUpdateCheck *self);

G_END_DECLS
