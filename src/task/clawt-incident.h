/*
 * clawt-incident.h - Failed runs, reported to whoever leads the team
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A delegated task that failed or stalled, an agent that could not start
 * with work waiting for it, and a routine run that did not happen each
 * used to leave one line in the room it died in and nothing anywhere
 * else.  Somebody found it hours later by reading every room.
 *
 * #ClawtIncidentDesk turns each into an *incident*: one record per run
 * that broke (the same task failing again updates it rather than opening
 * another), filed in a room per team called "Problems to look at", and
 * delivered to the team's lead -- or the chief of staff when the team has
 * none -- as a report it can act on with clawtilla_task_retry.  A lead
 * that is mid-turn has the report held and handed over when its turn
 * ends; nothing is dropped.
 *
 * The report never invites a reply.  It is a notice from clawtilla, not
 * a message from a peer, and an AI CLI cannot end a turn without writing
 * something: a report that invited one would have the lead's sign-off
 * routed somewhere, and the failing agent's next failure would report
 * that.  See docs/orchestration.org, "Incidents".
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-types.h"

G_BEGIN_DECLS

/**
 * ClawtIncidentKind:
 * @CLAWT_INCIDENT_FAILED: the run ended with an error
 * @CLAWT_INCIDENT_STALLED: clawtilla stopped the run -- a watchdog, a
 *   room budget or the loop guard
 * @CLAWT_INCIDENT_UNSTARTABLE: the agent, or its computer, would not
 *   start while work was waiting for it
 * @CLAWT_INCIDENT_ROUTINE_FAILED: a routine run could not be started at
 *   all
 *
 * What kind of breakage an incident reports.
 *
 * Four rather than "failed", because they need different next steps from
 * whoever reads the report: a stalled run can simply be picked up again,
 * a failed one needs reading first, an agent that will not start needs a
 * person or a fixed setting before any retry can help, and a routine
 * that never ran has no task to retry at all.
 */
typedef enum {
	CLAWT_INCIDENT_FAILED = 0,
	CLAWT_INCIDENT_STALLED,
	CLAWT_INCIDENT_UNSTARTABLE,
	CLAWT_INCIDENT_ROUTINE_FAILED
} ClawtIncidentKind;

/**
 * ClawtIncidentRecipient:
 * @CLAWT_INCIDENT_TO_LEAD: the failing agent's team lead, or the chief of
 *   staff when the team has no lead
 * @CLAWT_INCIDENT_TO_CHIEF: always the chief of staff
 * @CLAWT_INCIDENT_TO_NONE: nobody's turn; the report is only recorded in
 *   the room, for the operator
 *
 * Who `orchestration.incidents.to` sends a report to.
 */
typedef enum {
	CLAWT_INCIDENT_TO_LEAD = 0,
	CLAWT_INCIDENT_TO_CHIEF,
	CLAWT_INCIDENT_TO_NONE
} ClawtIncidentRecipient;

GType clawt_incident_kind_get_type(void) G_GNUC_CONST;
GType clawt_incident_recipient_get_type(void) G_GNUC_CONST;

#define CLAWT_TYPE_INCIDENT_KIND      (clawt_incident_kind_get_type())
#define CLAWT_TYPE_INCIDENT_RECIPIENT (clawt_incident_recipient_get_type())

/**
 * CLAWT_INCIDENT_ROOM_PREFIX:
 *
 * What every incidents room's id starts with.  A colon cannot appear in
 * a declared room id, so a room the daemon owns can never collide with
 * one somebody made.
 */
#define CLAWT_INCIDENT_ROOM_PREFIX "incidents:"

/**
 * CLAWT_INCIDENT_FLEET_ROOM:
 *
 * The incidents room for agents on no team.  `+` is outside the id
 * alphabet, so no team can be called this.
 */
#define CLAWT_INCIDENT_FLEET_ROOM "incidents:+fleet"

/**
 * CLAWT_INCIDENT_ROOM_NAME:
 *
 * What every incidents room is called on screen.
 */
#define CLAWT_INCIDENT_ROOM_NAME "Problems to look at"

/**
 * CLAWT_INCIDENT_MAX_AGENT_RETRIES:
 *
 * How many times an *agent* may retry one incident's run.
 *
 * A crash loop with a lead that retries every report is a loop that
 * spends money on every turn of it, so the third report tells the lead
 * to stop and explain instead, and the tool refuses.  The operator's own
 * Retry is not counted against this: a person pressing a button has
 * looked.
 */
#define CLAWT_INCIDENT_MAX_AGENT_RETRIES (2)

/**
 * clawt_incident_recipient_count:
 *
 * How many settings `orchestration.incidents.to` has, so a client can
 * offer them without naming them.
 *
 * Returns: the count
 */
guint clawt_incident_recipient_count(void);

/**
 * clawt_incident_recipient_nth:
 * @n: an index below clawt_incident_recipient_count()
 *
 * Returns: the setting at @n, in the order a client should offer them
 */
ClawtIncidentRecipient clawt_incident_recipient_nth(guint n);

/**
 * clawt_incident_recipient_nick:
 * @recipient: a setting
 *
 * Returns: (transfer none): its spelling in clawtilla.yaml and on the wire
 */
const gchar *clawt_incident_recipient_nick(ClawtIncidentRecipient recipient);

/**
 * clawt_incident_recipient_label:
 * @recipient: a setting
 *
 * Returns: (transfer none): how a client describes it to a person
 */
const gchar *clawt_incident_recipient_label(ClawtIncidentRecipient recipient);

/**
 * clawt_incident_kind_label:
 * @kind: an incident kind
 *
 * One spelling of each kind for every surface -- the report, the CLI and
 * the three clients -- so "could not start" is not "unstartable" in one
 * place and "start failure" in another.
 *
 * Returns: (transfer none): a short phrase
 */
const gchar *clawt_incident_kind_label(ClawtIncidentKind kind);

/**
 * clawt_incident_room_id:
 * @team: (nullable): a team id, or %NULL for an agent on no team
 *
 * The incidents room for @team.  One room per team rather than one per
 * failure: the lead reads one conversation, and the operator has one
 * place to look.
 *
 * Returns: (transfer full): the room id
 */
gchar *clawt_incident_room_id(const gchar *team);

/**
 * clawt_incident_room_is:
 * @room_id: (nullable): a room id
 *
 * Returns: %TRUE if @room_id is an incidents room
 */
gboolean clawt_incident_room_is(const gchar *room_id);

/**
 * clawt_incident_fence:
 * @label: (nullable): the info string after the opening fence
 * @text: (nullable): what goes inside
 *
 * @text in a code fence that @text cannot close.
 *
 * The fence is one backtick longer than the longest run of backticks in
 * @text, and never shorter than three.  A fixed fence is a promise the
 * fenced text can break: a log line holding the same nine backticks ends
 * the block, and everything after it reads as the daemon's own words.
 *
 * Returns: (transfer full): the fenced block, ending in a newline
 */
gchar *clawt_incident_fence(const gchar *label, const gchar *text);

/**
 * clawt_incident_resolve_recipient:
 * @config: the fleet configuration
 * @agent_id: the agent whose run broke
 * @to: who the setting says reports go to
 * @out_team: (out) (optional) (transfer full) (nullable): @agent_id's
 *   team, or %NULL when it is on none
 *
 * Who should hear about @agent_id's broken run.
 *
 * The lead of @agent_id's team, falling back to the chief of staff; or
 * the chief alone; or nobody.  Never @agent_id itself -- a lead's own
 * failure goes to the chief, and the chief's own goes to nobody, since
 * telling an agent about the run it just failed is a turn spent reading
 * a fact it can no longer act on.
 *
 * Returns: (transfer none) (nullable): an agent id from @config, or %NULL
 *   when nobody should be told
 */
const gchar *clawt_incident_resolve_recipient(ClawtConfig             *config,
                                              const gchar             *agent_id,
                                              ClawtIncidentRecipient   to,
                                              gchar                  **out_team);

/* ── One incident ────────────────────────────────────────────────── */

/**
 * ClawtIncident:
 *
 * One run that broke, and everything done about it since.  Owned by the
 * #ClawtIncidentDesk that filed it; read through the getters.
 */
typedef struct _ClawtIncident ClawtIncident;

#define CLAWT_TYPE_INCIDENT (clawt_incident_get_type())

GType clawt_incident_get_type(void) G_GNUC_CONST;

/**
 * clawt_incident_ref:
 * @self: a #ClawtIncident
 *
 * Returns: (transfer full): @self
 */
ClawtIncident *clawt_incident_ref(ClawtIncident *self);

/**
 * clawt_incident_unref:
 * @self: (transfer full) (nullable): a #ClawtIncident
 */
void clawt_incident_unref(ClawtIncident *self);

/**
 * clawt_incident_get_id:
 * @self: a #ClawtIncident
 *
 * Returns: (transfer none): the id, `inc-...`
 */
const gchar *clawt_incident_get_id(ClawtIncident *self);

/**
 * clawt_incident_get_kind:
 * @self: a #ClawtIncident
 *
 * Returns: how the run broke, the last time it did
 */
ClawtIncidentKind clawt_incident_get_kind(ClawtIncident *self);

/**
 * clawt_incident_get_agent:
 * @self: a #ClawtIncident
 *
 * Returns: (transfer none): whose run broke
 */
const gchar *clawt_incident_get_agent(ClawtIncident *self);

/**
 * clawt_incident_get_team:
 * @self: a #ClawtIncident
 *
 * Returns: (transfer none) (nullable): that agent's team
 */
const gchar *clawt_incident_get_team(ClawtIncident *self);

/**
 * clawt_incident_get_room:
 * @self: a #ClawtIncident
 *
 * Returns: (transfer none): the incidents room it is filed in
 */
const gchar *clawt_incident_get_room(ClawtIncident *self);

/**
 * clawt_incident_get_run_room:
 * @self: a #ClawtIncident
 *
 * Returns: (transfer none) (nullable): the room the broken run was in
 */
const gchar *clawt_incident_get_run_room(ClawtIncident *self);

/**
 * clawt_incident_get_task_id:
 * @self: a #ClawtIncident
 *
 * Returns: (transfer none) (nullable): the task whose run broke, the
 *   latest one for a routine
 */
const gchar *clawt_incident_get_task_id(ClawtIncident *self);

/**
 * clawt_incident_get_routine_id:
 * @self: a #ClawtIncident
 *
 * Returns: (transfer none) (nullable): the routine, when the run was one
 */
const gchar *clawt_incident_get_routine_id(ClawtIncident *self);

/**
 * clawt_incident_get_reason:
 * @self: a #ClawtIncident
 *
 * Returns: (transfer none) (nullable): what the run said when it stopped,
 *   as written -- untrusted, and not escaped
 */
const gchar *clawt_incident_get_reason(ClawtIncident *self);

/**
 * clawt_incident_get_log_lines:
 * @self: a #ClawtIncident
 *
 * Returns: (transfer none) (nullable) (array zero-terminated=1): the
 *   agent's last log lines when it broke
 */
const gchar *const *clawt_incident_get_log_lines(ClawtIncident *self);

/**
 * clawt_incident_get_recipient:
 * @self: a #ClawtIncident
 *
 * Returns: (transfer none) (nullable): who the report went, or is going,
 *   to; %NULL when it is recorded for the operator only
 */
const gchar *clawt_incident_get_recipient(ClawtIncident *self);

/**
 * clawt_incident_get_note:
 * @self: a #ClawtIncident
 *
 * Why the report did not go to an agent, when it did not: the setting
 * says nobody, the team has no lead and there is no chief, or the agent
 * that delegated the task was told by its settle notice already.
 *
 * Returns: (transfer none) (nullable): the sentence
 */
const gchar *clawt_incident_get_note(ClawtIncident *self);

/**
 * clawt_incident_get_occurrences:
 * @self: a #ClawtIncident
 *
 * Returns: how many times this run has broken while the incident was open
 */
guint clawt_incident_get_occurrences(ClawtIncident *self);

/**
 * clawt_incident_get_retries:
 * @self: a #ClawtIncident
 *
 * Returns: how many times it has been retried
 */
guint clawt_incident_get_retries(ClawtIncident *self);

/**
 * clawt_incident_get_agent_retries:
 * @self: a #ClawtIncident
 *
 * Returns: how many of those an agent asked for, which is what
 *   %CLAWT_INCIDENT_MAX_AGENT_RETRIES bounds
 */
guint clawt_incident_get_agent_retries(ClawtIncident *self);

/**
 * clawt_incident_get_retried_by:
 * @self: a #ClawtIncident
 *
 * Returns: (transfer none) (nullable): who retried it last -- an agent id,
 *   or "user" for the operator
 */
const gchar *clawt_incident_get_retried_by(ClawtIncident *self);

/**
 * clawt_incident_get_opened_at:
 * @self: a #ClawtIncident
 *
 * Returns: when it was opened, in microseconds since the epoch
 */
gint64 clawt_incident_get_opened_at(ClawtIncident *self);

/**
 * clawt_incident_get_updated_at:
 * @self: a #ClawtIncident
 *
 * Returns: when anything about it last changed, in microseconds
 */
gint64 clawt_incident_get_updated_at(ClawtIncident *self);

/**
 * clawt_incident_is_queued:
 * @self: a #ClawtIncident
 *
 * Returns: %TRUE while a report is waiting for its recipient to finish a
 *   turn, or for the router to accept it
 */
gboolean clawt_incident_is_queued(ClawtIncident *self);

/**
 * clawt_incident_get_delivered_at:
 * @self: a #ClawtIncident
 *
 * Returns: when a report last reached its recipient's mailbox, in
 *   microseconds, or 0 if none has
 */
gint64 clawt_incident_get_delivered_at(ClawtIncident *self);

/**
 * clawt_incident_is_resolved:
 * @self: a #ClawtIncident
 *
 * Returns: %TRUE once the run it reports finished, or was cancelled
 */
gboolean clawt_incident_is_resolved(ClawtIncident *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtIncident, clawt_incident_unref)

/* ── The desk ────────────────────────────────────────────────────── */

/**
 * ClawtIncidentRoutineOfFunc:
 * @task_id: a task
 * @user_data: what was registered with the hook
 *
 * Returns: (transfer none) (nullable): the routine whose run @task_id is,
 *   or %NULL when it is not a routine run
 */
typedef const gchar *(*ClawtIncidentRoutineOfFunc)(const gchar *task_id,
                                                   gpointer     user_data);

/**
 * ClawtIncidentRerunFunc:
 * @routine_id: the routine to run again
 * @user_data: what was registered with the hook
 * @error: (out) (optional): why it did not start
 *
 * Returns: (transfer full) (nullable): the new run's task id
 */
typedef gchar *(*ClawtIncidentRerunFunc)(const gchar  *routine_id,
                                         gpointer      user_data,
                                         GError      **error);

#define CLAWT_TYPE_INCIDENT_DESK (clawt_incident_desk_get_type())

G_DECLARE_FINAL_TYPE(ClawtIncidentDesk, clawt_incident_desk, CLAWT,
                     INCIDENT_DESK, GObject)

/**
 * clawt_incident_desk_new:
 * @config: the fleet configuration
 * @agents: the agents
 * @rooms: where incidents rooms are made
 * @router: what delivers reports
 * @tasks: the tasks whose failures it watches
 *
 * Watches @tasks from the moment it exists: a task reaching `failed` or
 * `stalled` opens (or updates) an incident, and one reaching `completed`
 * or `cancelled` resolves it.  The desk holds a reference to each
 * argument.
 *
 * Returns: (transfer full): a new #ClawtIncidentDesk
 */
ClawtIncidentDesk *clawt_incident_desk_new(ClawtConfig        *config,
                                           ClawtAgentManager  *agents,
                                           ClawtRoomManager   *rooms,
                                           ClawtMailboxRouter *router,
                                           ClawtTaskManager   *tasks);

/**
 * clawt_incident_desk_set_config:
 * @self: a #ClawtIncidentDesk
 * @config: a reloaded configuration
 */
void clawt_incident_desk_set_config(ClawtIncidentDesk *self,
                                    ClawtConfig       *config);

/**
 * clawt_incident_desk_set_event_bus:
 * @self: a #ClawtIncidentDesk
 * @bus: (nullable): where `incident.*` events are published
 */
void clawt_incident_desk_set_event_bus(ClawtIncidentDesk *self,
                                       ClawtEventBus     *bus);

/**
 * clawt_incident_desk_set_routine_hooks:
 * @self: a #ClawtIncidentDesk
 * @routine_of: (nullable) (scope forever): which routine a task is a run of
 * @rerun: (nullable) (scope forever): runs a routine again
 * @user_data: passed to both
 *
 * Routines belong to the daemon, so the desk asks rather than knowing.
 * Unset, a routine's failed run is reported as an ordinary task and
 * retried by re-queueing it.
 */
void clawt_incident_desk_set_routine_hooks(ClawtIncidentDesk          *self,
                                           ClawtIncidentRoutineOfFunc  routine_of,
                                           ClawtIncidentRerunFunc      rerun,
                                           gpointer                    user_data);

/**
 * clawt_incident_desk_is_enabled:
 * @self: a #ClawtIncidentDesk
 *
 * Returns: what `orchestration.incidents.enabled` says
 */
gboolean clawt_incident_desk_is_enabled(ClawtIncidentDesk *self);

/**
 * clawt_incident_desk_quiet_begin:
 * @self: a #ClawtIncidentDesk
 *
 * Stops failures being filed until the matching
 * clawt_incident_desk_quiet_end().  For a person stopping an agent: its
 * tasks fail with "the agent stopped", which is true and is not a
 * problem anybody needs a report about -- they just did it.  Nests.
 */
void clawt_incident_desk_quiet_begin(ClawtIncidentDesk *self);

/**
 * clawt_incident_desk_quiet_end:
 * @self: a #ClawtIncidentDesk
 */
void clawt_incident_desk_quiet_end(ClawtIncidentDesk *self);

/**
 * clawt_incident_desk_raise:
 * @self: a #ClawtIncidentDesk
 * @kind: how the run broke
 * @agent_id: whose run it was
 * @task_id: (nullable): the task, when there is one
 * @routine_id: (nullable): the routine, when the run was one
 * @reason: (nullable): what the run said when it stopped
 *
 * Files a broken run.
 *
 * An open incident about the same run -- the same routine, else the same
 * task -- is updated rather than a second one opened, and its report is
 * delivered again only when it was retried since the last one reached
 * the recipient: a retry that failed is news, the same failure counted
 * again is not.
 *
 * Returns: (transfer none) (nullable): the incident, or %NULL when
 *   incidents are switched off
 */
ClawtIncident *clawt_incident_desk_raise(ClawtIncidentDesk *self,
                                         ClawtIncidentKind  kind,
                                         const gchar       *agent_id,
                                         const gchar       *task_id,
                                         const gchar       *routine_id,
                                         const gchar       *reason);

/**
 * clawt_incident_desk_note_unstartable:
 * @self: a #ClawtIncidentDesk
 * @agent_id: the agent that would not start
 * @reason: (nullable): why
 *
 * One incident per unfinished task waiting on @agent_id.  An agent with
 * no work waiting raises nothing here -- its state already says it is in
 * error, and an incident is about a run.
 *
 * Returns: how many incidents were filed or updated
 */
guint clawt_incident_desk_note_unstartable(ClawtIncidentDesk *self,
                                           const gchar       *agent_id,
                                           const gchar       *reason);

/**
 * clawt_incident_desk_flush:
 * @self: a #ClawtIncidentDesk
 * @recipient_id: (nullable): only reports for this agent, or %NULL for all
 *
 * Delivers every report that is waiting and can go now: one message per
 * recipient and room, holding every report that waited.  A recipient
 * that is mid-turn keeps its reports until it is not.
 *
 * Called when a turn ends and from the daemon's sweep.
 *
 * Returns: how many incidents were delivered
 */
guint clawt_incident_desk_flush(ClawtIncidentDesk *self,
                                const gchar       *recipient_id);

/**
 * clawt_incident_desk_list:
 * @self: a #ClawtIncidentDesk
 *
 * Returns: (transfer container) (element-type ClawtIncident): every
 *   incident the desk still holds, newest first
 */
GPtrArray *clawt_incident_desk_list(ClawtIncidentDesk *self);

/**
 * clawt_incident_desk_get:
 * @self: a #ClawtIncidentDesk
 * @incident_id: (nullable): an incident id
 *
 * Returns: (transfer none) (nullable): the incident
 */
ClawtIncident *clawt_incident_desk_get(ClawtIncidentDesk *self,
                                       const gchar       *incident_id);

/**
 * clawt_incident_desk_find_for_task:
 * @self: a #ClawtIncidentDesk
 * @task_id: (nullable): a task id
 *
 * Returns: (transfer none) (nullable): the open incident about @task_id
 */
ClawtIncident *clawt_incident_desk_find_for_task(ClawtIncidentDesk *self,
                                                 const gchar       *task_id);

/**
 * clawt_incident_desk_may_retry:
 * @self: a #ClawtIncidentDesk
 * @caller: the agent asking
 * @task_id: (nullable): the task to retry
 * @incident_id: (nullable): or the incident whose run to retry
 * @refusal: (out) (optional) (transfer full) (nullable): why not
 *
 * The gate `clawtilla_task_retry` applies, which is the assignment gate:
 * a retry puts work back on somebody's list, so whoever could not have
 * assigned it cannot retry it either.  Also refused past
 * %CLAWT_INCIDENT_MAX_AGENT_RETRIES.
 *
 * Returns: %TRUE if @caller may retry it
 */
gboolean clawt_incident_desk_may_retry(ClawtIncidentDesk  *self,
                                       const gchar        *caller,
                                       const gchar        *task_id,
                                       const gchar        *incident_id,
                                       gchar             **refusal);

/**
 * clawt_incident_desk_retry:
 * @self: a #ClawtIncidentDesk
 * @task_id: (nullable): the task to retry
 * @incident_id: (nullable): or the incident whose run to retry
 * @who: who is asking: an agent id, or "user" for the operator
 * @why: (nullable): what the retrier wants the assignee to know
 * @out_rerun: (out) (optional): %TRUE when a routine was run again rather
 *   than a task re-queued
 * @error: (out) (optional): why it was refused
 *
 * Re-queues a failed or stalled task with its original prompt, or runs a
 * routine again, and records who asked.  Refuses a task in any other
 * state: a running task has nothing to retry, and a finished one would
 * be redone.  Permission is the caller's business --
 * clawt_incident_desk_may_retry() for an agent; the operator may retry
 * anything.
 *
 * Returns: (transfer full) (nullable): the task id now carrying the work
 */
gchar *clawt_incident_desk_retry(ClawtIncidentDesk  *self,
                                 const gchar        *task_id,
                                 const gchar        *incident_id,
                                 const gchar        *who,
                                 const gchar        *why,
                                 gboolean           *out_rerun,
                                 GError            **error);

G_END_DECLS
