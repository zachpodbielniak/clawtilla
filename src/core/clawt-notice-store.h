/*
 * clawt-notice-store.h - The fleet's notifications, held once
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Every client used to build its own alert list out of the events it
 * happened to receive, and keep it in memory.  So a notice cleared in
 * the GTK window stayed lit in the browser, a restart forgot everything,
 * and a client that connected after something went wrong never heard of
 * it at all.  The list is held here, by the daemon, in
 * `<state_dir>/notices.db`, and every client is a view of it.
 *
 * Two tables:
 *
 * - **notices**: one row per event the daemon rated an error or a notice
 *   (clawt_alert_tier_for_event()), worded once by
 *   clawt_alert_text_for_event().  A row can be read (it no longer counts
 *   on a badge) and dismissed (it no longer shows); neither deletes it --
 *   pruning does, by count and by age.
 * - **mutes**: the agents whose notifications are silenced, each until a
 *   time or until unmuted.  A muted agent's notices still arrive, already
 *   read, so the list stays a complete record; what a mute takes away is
 *   the interruption.
 *
 * Every time here is in seconds since the epoch, and every call that
 * depends on the clock takes `now` rather than reading it, so a timed
 * mute lapsing and a prune by age can be tested without waiting.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

#include "core/clawt-event.h"

G_BEGIN_DECLS

/**
 * CLAWT_NOTICES_KEEP_DEFAULT:
 *
 * How many notices are kept when `notices.keep` is not set.
 */
#define CLAWT_NOTICES_KEEP_DEFAULT (500)

/**
 * CLAWT_NOTICES_KEEP_DAYS_DEFAULT:
 *
 * How many days a notice is kept when `notices.keep_days` is not set.
 */
#define CLAWT_NOTICES_KEEP_DAYS_DEFAULT (30)

/**
 * ClawtNotice:
 * @id: the row's id, newest highest
 * @agent: (nullable): who it is about, for narrowing and muting
 * @tier: %CLAWT_ALERT_ERROR or %CLAWT_ALERT_NOTICE
 * @event_kind: the event it was made from
 * @source: where to look, for the row's caption (see
 *   clawt_alert_text_for_event())
 * @subject: (nullable): the event's subject
 * @title: what happened, in words
 * @created_at: when the event happened, seconds
 * @read: whether it has been seen; an unread one counts on the bell
 * @dismissed: whether it has been dismissed or cleared
 *
 * One notification.
 *
 * Since: 0.52.0
 */
typedef struct _ClawtNotice ClawtNotice;

struct _ClawtNotice {
    gint64          id;
    gchar          *agent;
    ClawtAlertTier  tier;
    gchar          *event_kind;
    gchar          *source;
    gchar          *subject;
    gchar          *title;
    gint64          created_at;
    gboolean        read;
    gboolean        dismissed;
};

GType clawt_notice_get_type(void) G_GNUC_CONST;
#define CLAWT_TYPE_NOTICE (clawt_notice_get_type())

/**
 * clawt_notice_copy:
 * @self: a #ClawtNotice
 *
 * Returns: (transfer full): a copy of @self
 */
ClawtNotice *clawt_notice_copy(const ClawtNotice *self);

/**
 * clawt_notice_free:
 * @self: (transfer full) (nullable): a #ClawtNotice
 */
void clawt_notice_free(ClawtNotice *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtNotice, clawt_notice_free)

/**
 * clawt_notice_tier_nick:
 * @tier: a tier
 *
 * The word a tier is stored and sent as: "error", "notice", "routine",
 * or "skip".
 *
 * Returns: (transfer none): the nick
 *
 * Since: 0.52.0
 */
const gchar *clawt_notice_tier_nick(ClawtAlertTier tier);

/**
 * clawt_notice_tier_from_nick:
 * @nick: (nullable): a nick from clawt_notice_tier_nick()
 *
 * Returns: the tier; %CLAWT_ALERT_NOTICE for anything unrecognised,
 *   which is the quieter of the two a stored row can hold
 *
 * Since: 0.52.0
 */
ClawtAlertTier clawt_notice_tier_from_nick(const gchar *nick);

/**
 * ClawtNoticeMute:
 * @agent: the agent
 * @until: when the mute lapses, seconds; 0 for "until unmuted"
 *
 * One muted agent.
 *
 * Since: 0.52.0
 */
typedef struct _ClawtNoticeMute ClawtNoticeMute;

struct _ClawtNoticeMute {
    gchar  *agent;
    gint64  until;
};

GType clawt_notice_mute_get_type(void) G_GNUC_CONST;
#define CLAWT_TYPE_NOTICE_MUTE (clawt_notice_mute_get_type())

/**
 * clawt_notice_mute_copy:
 * @self: a #ClawtNoticeMute
 *
 * Returns: (transfer full): a copy of @self
 */
ClawtNoticeMute *clawt_notice_mute_copy(const ClawtNoticeMute *self);

/**
 * clawt_notice_mute_free:
 * @self: (transfer full) (nullable): a #ClawtNoticeMute
 */
void clawt_notice_mute_free(ClawtNoticeMute *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtNoticeMute, clawt_notice_mute_free)

/**
 * clawt_notice_mute_for_parse:
 * @text: (nullable): how long, as `90s`, `30m`, `2h` or `3d`; a bare
 *   number is minutes
 * @out_seconds: (out): the length, in seconds
 *
 * Reads the length `clawtilla mute AGENT --for 2h` takes.  A mute is
 * sent to the daemon as the time it lapses, so the CLI turns this into
 * now plus the length -- the daemon never has to guess which clock
 * "two hours" was measured on.
 *
 * Returns: %TRUE for a positive length; %FALSE for anything else, with
 *   @out_seconds 0
 *
 * Since: 0.52.0
 */
gboolean clawt_notice_mute_for_parse(const gchar *text,
                                     gint64      *out_seconds);

/**
 * clawt_notice_list_badge:
 * @reply: (nullable): a `notice.list` reply's payload
 *
 * The number on a client's bell: unread conversations, decisions
 * waiting, Grok jobs needing attention, and unread alerts -- every one
 * already less the muted agents', because the daemon counted them.
 *
 * Added up here rather than taken from the reply's `total` alone so a
 * client and a test read it the same way, and a reply from a daemon that
 * left a member out counts that member as 0 rather than failing.
 *
 * Returns: the count
 *
 * Since: 0.52.0
 */
guint clawt_notice_list_badge(JsonObject *reply);

/**
 * clawt_notice_list_mutes:
 * @reply: (nullable): a `notice.list` reply's payload
 *
 * The muted agents a reply names, as the set clawt_unread_total() takes.
 *
 * Returns: (transfer full) (element-type utf8 gpointer): agent id ->
 *   itself; empty for no reply
 *
 * Since: 0.52.0
 */
GHashTable *clawt_notice_list_mutes(JsonObject *reply);

#define CLAWT_TYPE_NOTICE_STORE (clawt_notice_store_get_type())

G_DECLARE_FINAL_TYPE(ClawtNoticeStore, clawt_notice_store, CLAWT,
                     NOTICE_STORE, GObject)

/**
 * clawt_notice_store_new:
 * @path: the database file, usually `<state_dir>/notices.db`
 * @error: (out) (optional): return location for a #GError
 *
 * Opens the store, creating the file when there is none.
 *
 * Returns: (transfer full) (nullable): the store, or %NULL with @error
 *
 * Since: 0.52.0
 */
ClawtNoticeStore *clawt_notice_store_new(const gchar *path, GError **error);

/**
 * clawt_notice_store_add:
 * @self: a #ClawtNoticeStore
 * @notice: the notice; its @id, @read and @dismissed are ignored
 * @read: whether it arrives already read (its agent is muted)
 * @error: (out) (optional): return location for a #GError
 *
 * Records one notice, never dismissed.
 *
 * Returns: the new row's id, or 0 with @error
 *
 * Since: 0.52.0
 */
gint64 clawt_notice_store_add(ClawtNoticeStore   *self,
                              const ClawtNotice  *notice,
                              gboolean            read,
                              GError            **error);

/**
 * clawt_notice_store_get:
 * @self: a #ClawtNoticeStore
 * @id: a notice's id
 *
 * Returns: (transfer full) (nullable): the notice, or %NULL when there is
 *   none by that id
 *
 * Since: 0.52.0
 */
ClawtNotice *clawt_notice_store_get(ClawtNoticeStore *self, gint64 id);

/**
 * clawt_notice_store_list:
 * @self: a #ClawtNoticeStore
 * @agent: (nullable): only this agent's, or %NULL for every one
 * @include_dismissed: whether dismissed notices are listed too
 * @limit: at most this many; 0 for every one
 *
 * Returns: (transfer full) (element-type ClawtNotice): the notices,
 *   newest first
 *
 * Since: 0.52.0
 */
GPtrArray *clawt_notice_store_list(ClawtNoticeStore *self,
                                   const gchar      *agent,
                                   gboolean          include_dismissed,
                                   guint             limit);

/**
 * clawt_notice_store_mark_read:
 * @self: a #ClawtNoticeStore
 * @id: one notice, or 0 for every one
 *
 * Marks one notice, or all of them, read.
 *
 * Returns: how many changed; 0 for an unknown id
 *
 * Since: 0.52.0
 */
guint clawt_notice_store_mark_read(ClawtNoticeStore *self, gint64 id);

/**
 * clawt_notice_store_dismiss:
 * @self: a #ClawtNoticeStore
 * @id: a notice's id
 *
 * Dismisses one notice: it stops showing, and stops counting -- a
 * dismissed notice is read by definition.
 *
 * Returns: %TRUE if there was such a notice not already dismissed
 *
 * Since: 0.52.0
 */
gboolean clawt_notice_store_dismiss(ClawtNoticeStore *self, gint64 id);

/**
 * clawt_notice_store_clear:
 * @self: a #ClawtNoticeStore
 * @agent: (nullable): only this agent's, or %NULL for every one
 *
 * Dismisses every notice still showing, or every one of @agent's.
 *
 * Returns: how many were dismissed
 *
 * Since: 0.52.0
 */
guint clawt_notice_store_clear(ClawtNoticeStore *self, const gchar *agent);

/**
 * clawt_notice_store_count_unread:
 * @self: a #ClawtNoticeStore
 * @muted: (nullable) (element-type utf8 gpointer): agents whose notices
 *   are not counted
 *
 * Returns: how many notices are neither read nor dismissed, less the
 *   muted agents' -- the count a bell shows
 *
 * Since: 0.52.0
 */
guint clawt_notice_store_count_unread(ClawtNoticeStore *self,
                                      GHashTable       *muted);

/**
 * clawt_notice_store_prune:
 * @self: a #ClawtNoticeStore
 * @keep: how many of the newest to keep; 0 keeps every one
 * @keep_days: how many days to keep any; 0 keeps them for ever
 * @now: the time, seconds
 *
 * Drops notices past either limit, and mutes that have lapsed.
 *
 * Returns: how many notices were dropped
 *
 * Since: 0.52.0
 */
guint clawt_notice_store_prune(ClawtNoticeStore *self,
                               guint             keep,
                               guint             keep_days,
                               gint64            now);

/**
 * clawt_notice_store_count:
 * @self: a #ClawtNoticeStore
 *
 * Returns: how many notices are held, dismissed or not
 *
 * Since: 0.52.0
 */
guint clawt_notice_store_count(ClawtNoticeStore *self);

/**
 * clawt_notice_store_mute:
 * @self: a #ClawtNoticeStore
 * @agent: the agent
 * @until: when the mute lapses, seconds; 0 for "until unmuted"
 * @error: (out) (optional): return location for a #GError
 *
 * Mutes an agent, replacing any mute it had.
 *
 * Returns: %TRUE unless the write failed
 *
 * Since: 0.52.0
 */
gboolean clawt_notice_store_mute(ClawtNoticeStore  *self,
                                 const gchar       *agent,
                                 gint64             until,
                                 GError           **error);

/**
 * clawt_notice_store_unmute:
 * @self: a #ClawtNoticeStore
 * @agent: the agent
 *
 * Returns: %TRUE if @agent had a mute
 *
 * Since: 0.52.0
 */
gboolean clawt_notice_store_unmute(ClawtNoticeStore *self,
                                   const gchar      *agent);

/**
 * clawt_notice_store_is_muted:
 * @self: a #ClawtNoticeStore
 * @agent: (nullable): the agent
 * @now: the time, seconds
 *
 * Whether @agent is muted at @now.  A timed mute whose `until` has come
 * is not, whether or not a prune has removed it yet.
 *
 * Returns: %TRUE if it is muted
 *
 * Since: 0.52.0
 */
gboolean clawt_notice_store_is_muted(ClawtNoticeStore *self,
                                     const gchar      *agent,
                                     gint64            now);

/**
 * clawt_notice_store_list_mutes:
 * @self: a #ClawtNoticeStore
 * @now: the time, seconds
 *
 * Returns: (transfer full) (element-type ClawtNoticeMute): the mutes in
 *   force at @now, by agent
 *
 * Since: 0.52.0
 */
GPtrArray *clawt_notice_store_list_mutes(ClawtNoticeStore *self,
                                         gint64            now);

/**
 * clawt_notice_store_muted_set:
 * @self: a #ClawtNoticeStore
 * @now: the time, seconds
 *
 * The mutes in force, as a set -- what clawt_notice_store_count_unread()
 * and clawt_unread_total() take.
 *
 * Returns: (transfer full) (element-type utf8 gpointer): agent id -> itself
 *
 * Since: 0.52.0
 */
GHashTable *clawt_notice_store_muted_set(ClawtNoticeStore *self,
                                         gint64            now);

/**
 * clawt_notice_store_next_lapse:
 * @self: a #ClawtNoticeStore
 * @now: the time, seconds
 *
 * When the soonest timed mute lapses, so the daemon can say so the
 * moment it does rather than at the next prune.
 *
 * Returns: the time, seconds, or 0 when no timed mute is in force
 *
 * Since: 0.52.0
 */
gint64 clawt_notice_store_next_lapse(ClawtNoticeStore *self, gint64 now);

G_END_DECLS
