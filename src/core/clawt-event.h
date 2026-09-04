/*
 * clawt-event.h - Something that happened in the fleet
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

#include "clawt-types.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_EVENT (clawt_event_get_type())

GType clawt_event_get_type(void) G_GNUC_CONST;

/**
 * clawt_event_new:
 * @kind: a dotted kind, e.g. `agent.state-changed`
 * @subject: (nullable): what it happened to -- an agent id, room id or task id
 *
 * Creates an event.
 *
 * Returns: (transfer full): a new #ClawtEvent
 */
ClawtEvent *clawt_event_new(const gchar *kind, const gchar *subject);

ClawtEvent *clawt_event_copy(ClawtEvent *self);
void        clawt_event_free(ClawtEvent *self);

const gchar *clawt_event_get_kind(ClawtEvent *self);
const gchar *clawt_event_get_subject(ClawtEvent *self);
gint64       clawt_event_get_timestamp(ClawtEvent *self);

/**
 * clawt_event_set_timestamp:
 * @self: a #ClawtEvent
 * @timestamp: microseconds since the epoch
 *
 * Overrides when the event happened.  Used when reading an event back off
 * disk, so a replayed event keeps its original time rather than claiming
 * to have happened when it was read.
 */
void clawt_event_set_timestamp(ClawtEvent *self, gint64 timestamp);

/**
 * clawt_event_get_cursor:
 * @self: a #ClawtEvent
 *
 * The event's position in the stream.
 *
 * A client that reconnects asks to resume from the last cursor it saw.
 * Sequence numbers rather than timestamps, because two events in the same
 * microsecond are ordinary and a client must not have to guess which of
 * them it already has.
 *
 * Returns: the sequence number, or 0 if it has not been published
 */
guint64 clawt_event_get_cursor(ClawtEvent *self);

void clawt_event_set_cursor(ClawtEvent *self, guint64 cursor);

/**
 * clawt_event_set_detail:
 * @self: a #ClawtEvent
 * @key: field name
 * @value: (nullable): field value
 *
 * Attaches a string field.
 */
void clawt_event_set_detail(ClawtEvent  *self,
                            const gchar *key,
                            const gchar *value);

void clawt_event_set_detail_int(ClawtEvent  *self,
                                const gchar *key,
                                gint64       value);

const gchar *clawt_event_get_detail(ClawtEvent *self, const gchar *key);

/**
 * clawt_event_get_detail_int:
 * @self: a #ClawtEvent
 * @key: a detail set with clawt_event_set_detail_int()
 *
 * Details travel as strings, so this is the counterpart that turns one
 * back -- without it every caller writes the same g_ascii_strtoll().
 *
 * Returns: the value, or 0 when there is no such detail or it is not a
 *   number
 */
gint64 clawt_event_get_detail_int(ClawtEvent *self, const gchar *key);

/**
 * clawt_event_to_json:
 * @self: a #ClawtEvent
 *
 * Renders the event for the wire and for the event log.
 *
 * Returns: (transfer full): a JSON object node
 */
JsonNode *clawt_event_to_json(ClawtEvent *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtEvent, clawt_event_free)

/**
 * ClawtAlertTier:
 * @CLAWT_ALERT_SKIP: not worth a row at all
 * @CLAWT_ALERT_ERROR: something failed
 * @CLAWT_ALERT_NOTICE: something degraded, or is worth knowing
 * @CLAWT_ALERT_ROUTINE: the ordinary stream, drawn quietly
 *
 * How loudly an event is drawn in a client's alerts surface.
 */
typedef enum {
    CLAWT_ALERT_SKIP,
    CLAWT_ALERT_ERROR,
    CLAWT_ALERT_NOTICE,
    CLAWT_ALERT_ROUTINE
} ClawtAlertTier;

/**
 * clawt_alert_tier_for_event:
 * @event: the event
 *
 * Which tier an event belongs in.
 *
 * Only the first two tiers are coloured and only they are counted on a
 * bell: if everything carries a colour, colour stops meaning anything,
 * and routine entries are the majority the moment a filter widens.
 *
 * `image.progress` and `agent.typing` are skipped outright -- a download
 * emits one entry per percent and would fill the whole list with one
 * file, and typing is a spinner rather than something that happened.
 *
 * An agent that stopped when nobody asked it to is the one routine event
 * that is not routine, so `agent.state` reaching `error` or `degraded`
 * is a notice rather than an error: it may well have been asked to.
 *
 * It takes the event rather than a kind and a loose string, because
 * which detail decides the tier differs per kind -- `agent.state` reads
 * `state` and `image.finished` reads `error` -- and a caller passing the
 * wrong one would classify silently and wrongly.
 *
 * Here rather than in either client because both classify the same
 * stream, and two implementations of one rule differ exactly once -- on
 * the kind somebody adds next.
 *
 * Returns: the tier
 */
ClawtAlertTier clawt_alert_tier_for_event(ClawtEvent *event);

/**
 * clawt_event_is_ephemeral:
 * @event: the event
 *
 * Whether @event describes work in flight rather than something that
 * happened.
 *
 * An ephemeral event is delivered live to every subscriber and then
 * forgotten: it is not kept in the bus's replay ring and not written to
 * the ndjson event log.  Two separate reasons, and both matter.
 *
 * The ring is small and shared.  A turn makes tens of tool calls, so
 * retaining steps would push real events out of it within one busy
 * turn -- and a client resuming from a cursor would be told its replay
 * was incomplete because an agent had been working, which is exactly
 * when it most needs the messages it missed.
 *
 * The log is the record.  A step is not part of the answer and is
 * deliberately never persisted anywhere, because the last thing in a
 * thread is what `clawt_task_manager_complete_on_turn_end()` reads as a
 * task's result -- a retained step would make "Ran 6 commands" the
 * recorded outcome of somebody's delegated work.
 *
 * A client that missed steps is caught up from `room.steps` instead,
 * which is the room's own bounded in-memory history and outlives the
 * turn -- the steps stay in the conversation where they happened. It
 * is the *event* that is not retained, not the step.
 *
 * Here rather than in the bus and the log separately, because the two
 * had no reason to disagree and every reason to be asked the same
 * question.
 *
 * Returns: %TRUE if @event must not be retained
 */
gboolean clawt_event_is_ephemeral(ClawtEvent *event);

/**
 * CLAWT_TOAST_REPEAT_SECONDS:
 *
 * How long an identical toast is treated as already said.
 *
 * Matched to how long libadwaita leaves an ordinary toast on screen, so
 * what this suppresses is only ever a copy of something the reader can
 * still see.
 */
#define CLAWT_TOAST_REPEAT_SECONDS 5

/**
 * clawt_toast_should_show:
 * @previous: (nullable): the last toast text, or %NULL if none
 * @previous_at_us: when that one was shown, from a monotonic clock
 * @text: (nullable): what is about to be said
 * @now_us: now, from the same clock
 *
 * Whether a toast is worth adding, or is a copy of one still on screen.
 *
 * A toast answers a question somebody is holding right now. A *polled*
 * request that keeps failing answers nothing and says the same sentence
 * once per refresh -- the screen panel did exactly that, raising "there
 * is no frame yet" over and over while a VM booted, above a panel
 * already saying so in place. The per-call fix was to stop calling that
 * a failure; this is the shape underneath, and it is here because the
 * next polled request to fail would otherwise do it again.
 *
 * Suppressed only while the first is plausibly still visible. A repeat
 * after that is news: something is still wrong, nobody is looking at the
 * old toast, and hiding it for ever would be worse than saying it twice.
 *
 * A monotonic clock, not a wall clock -- both ends of the comparison are
 * local, and a wall clock stepped backwards by an NTP correction would
 * silence every toast until it caught up.
 *
 * Empty text is never worth showing: a toast with nothing in it is a bar
 * that appears, covers a control and says nothing.
 *
 * Returns: %TRUE if it should be shown
 */
gboolean clawt_toast_should_show(const gchar *previous,
                                 gint64       previous_at_us,
                                 const gchar *text,
                                 gint64       now_us);

/**
 * clawt_alert_arrives_read:
 * @surface_showing: whether the alerts surface is in front of the reader
 * @tier: the alert's tier
 *
 * Whether an arriving alert should be recorded as already read.
 *
 * An unread count means "one you have not seen".  Both clients used to
 * record every alert unread and clear the lot on a show *transition*,
 * which is right for one that arrives while the surface is closed and
 * wrong for one that lands in front of somebody: it was inserted
 * unread, bumped the badge, and went on counting until the reader
 * closed the panel and opened it again -- a number pointing at a row
 * they had watched appear.
 *
 * The routine tier is never counted by either client, so it arrives
 * read whatever is on screen.  Saying that here rather than in each
 * badge keeps "what counts" and "what has been seen" one answer rather
 * than two that agree until somebody edits one.
 *
 * Pure, so both halves can be exercised without a window or a browser
 * -- including the positive control, an alert arriving while the
 * surface is closed, without which this would be a one-sided assertion
 * that passes in a build whose badge never counts anything.
 *
 * Returns: %TRUE if the alert has already been seen
 */
gboolean clawt_alert_arrives_read(gboolean       surface_showing,
                                  ClawtAlertTier tier);

/**
 * clawt_team_tally: (skip)
 * @agents: the agent array from a fleet reply
 * @team_id: (nullable): the team to count, or %NULL for the agents that
 *   are in none
 * @total: (out): how many agents are in it
 * @running: (out): how many of those are running
 * @busy: (out): how many of those are working right now
 *
 * What a team heading has to say about the team behind it.
 *
 * A folded team is one line standing in for everything under it, which
 * is the whole reason the tally exists.  What it said was running out of
 * total -- and agents are started once and stay running, so that number
 * barely moves, while the thing somebody wants off a glance is whether
 * the team is doing anything.  That was drawn on every agent row and
 * thrown away at the one level where the rows are not on screen.
 *
 * Here rather than in each client because it was written twice already,
 * with the out-parameters in a different order in each, and it is about
 * to grow a third count.  The GTK copy asked with a %NULL team and the
 * web copy with an empty string, against a member the daemon omits
 * entirely, so the two would have disagreed the first time anything
 * wrote "" into it.  %NULL and "" name the same group here.
 *
 * @busy is a subset of @running: a stopped agent is not mid-turn
 * whatever the reply says, and a heading must not claim otherwise.
 *
 * Counted from the same reply the rows are built from, so a folded
 * team's tally cannot disagree with what unfolding it shows.
 */
void clawt_team_tally(JsonArray   *agents,
                      const gchar *team_id,
                      guint       *total,
                      guint       *running,
                      guint       *busy);

G_END_DECLS
