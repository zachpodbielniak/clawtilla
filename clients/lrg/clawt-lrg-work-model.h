/*
 * clawt-lrg-work-model.h - Decisions behind the Work and Automation pages
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Everything the Routines, Triggers, Tasks and Decisions pages and the
 * Ctrl+K "Go to…" palette decide -- what a row says, which tone a badge
 * takes, what a schedule means, which payload an editor sends, which
 * destinations a search keeps -- lives here, with no libregnum header,
 * so tests/test-lrg-models-work.c can check it without a display.
 *
 * Every member read here was checked against the
 * json_builder_set_member_name() that produces it in src/core/, and
 * every boolean is read with the boolean reader: a string reader on a
 * JSON boolean once wrote `enabled: false` back over every routine
 * somebody opened and saved (AGENTS.md, "A member name is not checked
 * by anything").
 */

#pragma once

#include <clawtilla.h>
#include <json-glib/json-glib.h>

#include "clawt-lrg-model.h"

G_BEGIN_DECLS

/* ── Schema-driven fields (routines and triggers) ─────────────────── */

/**
 * ClawtLrgFieldKind:
 * @CLAWT_LRG_FIELD_TEXT: a one-line entry
 * @CLAWT_LRG_FIELD_SWITCH: an on/off switch
 * @CLAWT_LRG_FIELD_NUMBER: a whole number
 * @CLAWT_LRG_FIELD_LIST: a comma-separated list of strings
 *
 * The control an editor draws for a schema entry nobody gave a
 * dedicated one.
 */
typedef enum {
    CLAWT_LRG_FIELD_TEXT = 0,
    CLAWT_LRG_FIELD_SWITCH,
    CLAWT_LRG_FIELD_NUMBER,
    CLAWT_LRG_FIELD_LIST
} ClawtLrgFieldKind;

/**
 * ClawtLrgSchemaField:
 * @leaf: the member name on the wire, such as `isolate`
 * @kind: which control edits it
 * @hint: (transfer full): the first paragraph of its schema documentation
 *
 * One option of a routine or a trigger that the editor has no
 * dedicated control for.
 */
typedef struct {
    const gchar       *leaf;
    ClawtLrgFieldKind  kind;
    gchar             *hint;
} ClawtLrgSchemaField;

/**
 * clawt_lrg_work_extra_fields:
 * @prefix: the schema list, such as `routines.` or `triggers.`
 * @handled: (array zero-terminated=1): leaves the editor draws itself
 *
 * The leaves under @prefix the editor has no control of its own for,
 * walked from the schema.  The GTK routine editor names its keys by
 * hand and so never offered `isolate`; walking the schema means an
 * option added there reaches this editor from the day it exists.
 * Secrets and mappings are never offered: a secret is minted by the
 * daemon and posting one is refused.
 *
 * Returns: (transfer full) (element-type ClawtLrgSchemaField): the
 *   fields, in schema order
 */
GPtrArray *clawt_lrg_work_extra_fields(const gchar        *prefix,
                                       const gchar *const *handled);

/**
 * clawt_lrg_work_boolean:
 * @object: (nullable): a routine or trigger from a listing, or %NULL for
 *   a new one
 * @prefix: `routines.` or `triggers.`
 * @leaf: the member
 *
 * A boolean member read as a boolean, falling back to the schema's
 * default for that key -- never a default written here, which is how
 * the GTK editor came to start a new routine's `enabled` from a
 * different place than the daemon does.
 *
 * Returns: the value
 */
gboolean clawt_lrg_work_boolean(JsonObject  *object,
                                const gchar *prefix,
                                const gchar *leaf);

/**
 * clawt_lrg_work_joined:
 * @object: (nullable): an object
 * @member: an array-of-strings member
 *
 * Returns: (transfer full): the strings joined with ", "; "" for none
 */
gchar *clawt_lrg_work_joined(JsonObject *object, const gchar *member);

/**
 * clawt_lrg_work_field_label:
 * @leaf: a schema leaf such as `catch_up`
 *
 * Returns: (transfer full): "Catch up"
 */
gchar *clawt_lrg_work_field_label(const gchar *leaf);

/**
 * clawt_lrg_work_field_text:
 * @object: (nullable): the routine or trigger being edited
 * @leaf: the member
 * @kind: the field's control
 *
 * What a text control starts with: a number written out (read with the
 * int reader -- GTK read `jitter_seconds` with the string reader and so
 * always started it at its fallback), a list joined, a string as is.
 *
 * Returns: (transfer full): the text; "" when absent
 */
gchar *clawt_lrg_work_field_text(JsonObject        *object,
                                 const gchar       *leaf,
                                 ClawtLrgFieldKind  kind);

/**
 * clawt_lrg_work_field_value:
 * @kind: the field's control
 * @text: (nullable): what a text control holds
 * @active: what a switch holds
 * @error: (out) (optional): why the text is not a value of that kind
 *
 * The JSON value to send for a field, typed by the schema: a boolean
 * for a switch, a number for a number, an array for a list.  A setter
 * that wrote a list as a scalar once denied nothing (AGENTS.md, "A
 * setter must dispatch on entry->type").
 *
 * Returns: (transfer full) (nullable): the value
 */
JsonNode *clawt_lrg_work_field_value(ClawtLrgFieldKind   kind,
                                     const gchar        *text,
                                     gboolean            active,
                                     GError            **error);

/**
 * clawt_lrg_work_agent_choices:
 * @agents: (nullable): an `agent.list` reply
 * @ids: (out) (transfer full): agent ids, %NULL-terminated
 * @labels: (out) (transfer full): their names
 */
void clawt_lrg_work_agent_choices(JsonNode *agents, GStrv *ids,
                                  GStrv *labels);

/**
 * clawt_lrg_work_first_paragraph:
 * @doc: (nullable): a schema documentation string
 *
 * Returns: (transfer full): its first paragraph on one line
 */
gchar *clawt_lrg_work_first_paragraph(const gchar *doc);

/* ── Routines ─────────────────────────────────────────────────────── */

/**
 * clawt_lrg_schedule_choices:
 * @ids: (out) (transfer full): the schedule nicks, %NULL-terminated
 * @labels: (out) (transfer full): what each is called
 *
 * Walked from #ClawtSchedule, never spelled out here.
 */
void clawt_lrg_schedule_choices(GStrv *ids, GStrv *labels);

/**
 * clawt_lrg_weekday_choices:
 * @ids: (out) (transfer full): `sunday` … `saturday`, %NULL-terminated
 * @labels: (out) (transfer full): their names in the current locale
 *
 * The names come from GLib's own calendar rather than a list of seven
 * strings, so a translated desktop draws translated days while the ids
 * stay the ones the daemon's cron parser reads.
 */
void clawt_lrg_weekday_choices(GStrv *ids, GStrv *labels);

/**
 * ClawtLrgScheduleUses:
 * @at: the time of day is used
 * @minute_only: only the minute of @at matters (hourly)
 * @weekday: the day of the week is used
 * @cron: the expression is used
 *
 * Which fields of the editor a schedule reads.
 */
typedef struct {
    gboolean at;
    gboolean minute_only;
    gboolean weekday;
    gboolean cron;
} ClawtLrgScheduleUses;

/**
 * clawt_lrg_schedule_uses:
 * @schedule: (nullable): a schedule nick
 *
 * Returns: which fields @schedule reads; nothing for an unknown nick
 */
ClawtLrgScheduleUses clawt_lrg_schedule_uses(const gchar *schedule);

/**
 * clawt_lrg_parse_at:
 * @at: (nullable): a time of day such as `09:00`
 * @hour: (out): the hour
 * @minute: (out): the minute
 *
 * Whether @at can be shown by a time picker without changing it.  A
 * value the picker cannot hold is edited as text instead, because a
 * control that cannot represent the current value silently replaces it.
 *
 * Returns: %TRUE if @at is `H:MM` or `HH:MM` within a day
 */
gboolean clawt_lrg_parse_at(const gchar *at, gint *hour, gint *minute);

/**
 * clawt_lrg_schedule_preview:
 * @schedule: a schedule nick
 * @at: (nullable): the time of day
 * @weekday: (nullable): the day, for weekly
 * @cron: (nullable): the expression, for custom
 * @after: when to count from
 * @ok: (out) (optional): whether the schedule is one the daemon will take
 *
 * What the schedule means, in words a person can check: "Next: Mon 25
 * Aug 09:00, then Tue 26 Aug 09:00 and Wed 27 Aug 09:00", or why it is
 * not a schedule.  An expression is not something a person can check by
 * reading it; three dates are.  Worked out with the same
 * clawt_cron_from_preset() and clawt_cron_next() the daemon schedules
 * with, so the preview and the scheduler cannot disagree.
 *
 * Returns: (transfer full): the sentence
 */
gchar *clawt_lrg_schedule_preview(const gchar *schedule,
                                  const gchar *at,
                                  const gchar *weekday,
                                  const gchar *cron,
                                  GDateTime   *after,
                                  gboolean    *ok);

/**
 * clawt_lrg_routine_title:
 * @routine: a routine from `routine.list`
 *
 * Returns: (transfer none): its description, or its id
 */
const gchar *clawt_lrg_routine_title(JsonObject *routine);

/**
 * clawt_lrg_routine_when:
 * @routine: a routine from `routine.list`
 *
 * When it next runs: "next Mon 25 Aug at 09:00", "will never run — …"
 * for a schedule the daemon reported a `problem` with, "only when you
 * ask" for an enabled routine with no next run, "off" otherwise.  The
 * broken case is said rather than hidden: GTK once drew "only when you
 * ask" for it, which is what a deliberately manual routine says.
 *
 * Returns: (transfer full): the phrase
 */
gchar *clawt_lrg_routine_when(JsonObject *routine);

/**
 * clawt_lrg_routine_last_run:
 * @routine: a routine from `routine.list`
 * @now_us: the current time in microseconds
 * @tone: (out) (optional) (transfer none): the badge tone for it
 *
 * Returns: (transfer full) (nullable): "ok 2h ago", "failed 3d ago",
 *   or %NULL for a routine that has never run
 */
gchar *clawt_lrg_routine_last_run(JsonObject   *routine,
                                  gint64        now_us,
                                  const gchar **tone);

/**
 * ClawtLrgRoutineForm:
 * @creating: a new routine rather than an edit
 * @id: its name
 * @description: one line about it
 * @instructions: the whole prompt
 * @agent: (nullable): the agent id
 * @directory: where it runs
 * @schedule: the schedule nick
 * @at: the time of day
 * @weekday: the day, for weekly
 * @cron: the expression, for custom
 * @worktree: run in a git worktree
 * @enabled: runs on its schedule
 * @catch_up: run a missed one at startup
 * @read_only: read-only tools
 * @proactive: look for work unasked
 * @jitter: the jitter field as typed
 * @extras: (nullable): further schema members, already typed
 *
 * What the routine editor holds when Save is pressed.
 */
typedef struct {
    gboolean     creating;
    const gchar *id;
    const gchar *description;
    const gchar *instructions;
    const gchar *agent;
    const gchar *directory;
    const gchar *schedule;
    const gchar *at;
    const gchar *weekday;
    const gchar *cron;
    gboolean     worktree;
    gboolean     enabled;
    gboolean     catch_up;
    gboolean     read_only;
    gboolean     proactive;
    const gchar *jitter;
    JsonObject  *extras;
} ClawtLrgRoutineForm;

/**
 * clawt_lrg_routine_payload:
 * @form: the editor's values
 * @error: (out) (optional): why it was refused
 *
 * The `routine.add` / `routine.update` payload.  Refused here, before
 * anything is sent: a routine with no name, no instructions (it would
 * fire and ask an agent for nothing), no agent, a jitter that is not a
 * whole number of seconds, or a schedule clawt_cron_from_preset()
 * rejects -- the last because the daemon would refuse it anyway, and
 * a refusal that arrives with the form still open is the cheaper one.
 *
 * Returns: (transfer full) (nullable): the payload
 */
JsonNode *clawt_lrg_routine_payload(const ClawtLrgRoutineForm *form,
                                    GError                   **error);

/* ── Triggers ─────────────────────────────────────────────────────── */

/**
 * clawt_lrg_provider_choices:
 * @ids: (out) (transfer full): provider nicks, %NULL-terminated
 * @labels: (out) (transfer full): their labels
 *
 * Walked from clawt_trigger_provider_count() / _nth_nick() / _nth_label(),
 * so a provider added to the library reaches the editor at once and none
 * the daemon does not understand can be offered.
 */
void clawt_lrg_provider_choices(GStrv *ids, GStrv *labels);

/**
 * clawt_lrg_trigger_state:
 * @trigger: a trigger from `trigger.list`
 * @tone: (out) (optional) (transfer none): the badge tone
 *
 * One word, in priority order, rather than three flags a reader has to
 * combine: no secret, then waiting for its first delivery, then off,
 * then on.  "Why is this not firing" wants one answer, and the first
 * thing that is true is always it.
 *
 * Returns: (transfer none): the state phrase
 */
const gchar *clawt_lrg_trigger_state(JsonObject   *trigger,
                                     const gchar **tone);

/**
 * clawt_lrg_trigger_subtitle:
 * @trigger: a trigger from `trigger.list`
 *
 * Returns: (transfer full): "agent — provider", plus the extra
 *   recipients and the runs still unfinished when there are any
 */
gchar *clawt_lrg_trigger_subtitle(JsonObject *trigger);

/**
 * clawt_lrg_trigger_address:
 * @endpoint: (nullable): a trigger's endpoint
 * @listening: (nullable): the `listening` array of a `trigger.list` entry
 *
 * The address a forge should call: the whole URL when the receiver says
 * where it listens, else the path alone.
 *
 * Returns: (transfer full) (nullable): the address, or %NULL with no
 *   endpoint
 */
gchar *clawt_lrg_trigger_address(const gchar *endpoint,
                                 JsonArray   *listening);

/**
 * clawt_lrg_trigger_secret_body:
 * @reply: a `trigger.add` or `trigger.rotate` reply
 * @lead: what happened, said first
 * @listening: (nullable): where the receiver listens, from the listing
 *
 * The text above the secret in the "copy this now" dialog: @lead, the
 * address, and -- only when the daemon said `secret_shown_once`, never
 * because this client assumes it -- that this is the only time it is
 * shown and rotating is the way back.
 *
 * Returns: (transfer full) (nullable): the text, or %NULL when the reply
 *   carries no secret (an update), so there is nothing to show
 */
gchar *clawt_lrg_trigger_secret_body(JsonObject  *reply,
                                     const gchar *lead,
                                     JsonArray   *listening);

/**
 * clawt_lrg_trigger_report:
 * @reply: a `trigger.test` or `trigger.replay` reply
 *
 * Returns: (transfer none): the `report`, else the `prompt`, else ""
 */
const gchar *clawt_lrg_trigger_report(JsonObject *reply);

/**
 * clawt_lrg_delivery_title:
 * @delivery: one of `trigger.deliveries`
 *
 * Returns: (transfer full): "trigger — outcome"
 */
gchar *clawt_lrg_delivery_title(JsonObject *delivery);

/**
 * clawt_lrg_delivery_subtitle:
 * @delivery: one of `trigger.deliveries`
 *
 * `at` arrives as a string of Unix seconds (the store hands back text
 * columns), so it is parsed here rather than read with the int reader.
 *
 * Returns: (transfer full): "Mon 25 Aug 09:00 — push — detail"
 */
gchar *clawt_lrg_delivery_subtitle(JsonObject *delivery);

/**
 * ClawtLrgTriggerForm:
 * @creating: a new trigger rather than an edit
 * @id: its name
 * @description: one line about it
 * @instructions: the prompt template
 * @agent: (nullable): the primary agent
 * @agents: further recipients, comma separated
 * @room: where a run reports
 * @provider: the provider nick
 * @events: event names, comma separated
 * @repo: only this repository
 * @branch: only this branch
 * @header: the event header, for a generic sender
 * @directory: where the run works
 * @worktree: in a git worktree
 * @isolate: a conversation of its own
 * @enabled: whether it fires
 * @extras: (nullable): further schema members, already typed
 *
 * What the trigger editor holds when Save is pressed.
 */
typedef struct {
    gboolean     creating;
    const gchar *id;
    const gchar *description;
    const gchar *instructions;
    const gchar *agent;
    const gchar *agents;
    const gchar *room;
    const gchar *provider;
    const gchar *events;
    const gchar *repo;
    const gchar *branch;
    const gchar *header;
    const gchar *directory;
    gboolean     worktree;
    gboolean     isolate;
    gboolean     enabled;
    JsonObject  *extras;
} ClawtLrgTriggerForm;

/**
 * clawt_lrg_trigger_payload:
 * @form: the editor's values
 * @error: (out) (optional): why it was refused
 *
 * The `trigger.add` / `trigger.update` payload.  The lists go as JSON
 * arrays, split and trimmed here.  `enabled` is not sent for a new
 * trigger: the daemon starts every one off whatever was posted, and
 * sending TRUE would be a claim the reply contradicts.
 *
 * Returns: (transfer full) (nullable): the payload
 */
JsonNode *clawt_lrg_trigger_payload(const ClawtLrgTriggerForm *form,
                                    GError                   **error);

/* ── Tasks ────────────────────────────────────────────────────────── */

/**
 * ClawtLrgTaskRow:
 * @task: (transfer full): the task object
 * @depth: how far below a root it sits in this listing
 *
 * One row of the task tree.
 */
typedef struct {
    JsonObject *task;
    guint       depth;
} ClawtLrgTaskRow;

/**
 * clawt_lrg_task_tree:
 * @reply: (nullable): a `task.list` reply
 *
 * The tasks depth-first, each under the task named by its `parent`
 * member, in listing order among siblings.  A task whose parent is not
 * in the listing is a root here (its parent was pruned, or belongs to
 * nobody this listing covers), and a cycle -- which the daemon never
 * produces -- is broken rather than followed for ever.
 *
 * Returns: (transfer full) (element-type ClawtLrgTaskRow): the rows
 */
GPtrArray *clawt_lrg_task_tree(JsonNode *reply);

/**
 * clawt_lrg_task_title:
 * @task: a task
 *
 * Returns: (transfer full): "origin → assignee"
 */
gchar *clawt_lrg_task_title(JsonObject *task);

/**
 * clawt_lrg_task_tone:
 * @state: (nullable): a task state nick
 *
 * clawt_task_state_tone() for a nick; a state this build does not know
 * stays neutral rather than refusing, so a client one version behind
 * still draws the row.
 *
 * Returns: (transfer none): the tone
 */
const gchar *clawt_lrg_task_tone(const gchar *state);

/**
 * clawt_lrg_task_cancellable:
 * @state: (nullable): a task state nick
 *
 * Cancel is offered only while a task is going, because cancelling a
 * finished one is not a refusal the daemon needs to explain -- it is a
 * button that should not have been there.
 *
 * Returns: whether to offer Cancel
 */
gboolean clawt_lrg_task_cancellable(const gchar *state);

/**
 * clawt_lrg_task_note:
 * @task: a task
 *
 * The freshest thing anybody knows: the assignee's progress note while
 * the work runs, and the caveat once it ended without anybody saying so
 * (`result_inferred`, read as a boolean) -- a row that reads
 * `completed` and nothing else cannot tell a reported result from a
 * turn that simply stopped.
 *
 * Returns: (transfer none) (nullable): the note
 */
const gchar *clawt_lrg_task_note(JsonObject *task);

/* ── Decisions ────────────────────────────────────────────────────── */

/**
 * clawt_lrg_decision_items:
 * @reply: (nullable): a `decision.list` reply
 * @open: (out) (optional): the reply's `open` count
 *
 * Returns: (transfer none) (nullable): the `decisions` array
 */
JsonArray *clawt_lrg_decision_items(JsonNode *reply, guint *open);

/**
 * clawt_lrg_decision_going:
 * @decision: a decision
 *
 * Returns: (transfer full) (nullable): "Going ahead with: X — reason",
 *   or %NULL when it has no default
 */
gchar *clawt_lrg_decision_going(JsonObject *decision);

/**
 * clawt_lrg_decision_options:
 * @decision: a decision
 *
 * Returns: (transfer full): its non-empty options, %NULL-terminated
 */
GStrv clawt_lrg_decision_options(JsonObject *decision);

/**
 * clawt_lrg_decision_meta:
 * @decision: a decision
 * @now_us: the current time in microseconds
 *
 * Who asked and when.  `created_at` is Unix *seconds* on the wire,
 * every other timestamp a client draws is microseconds, and handing the
 * raw value to clawt_time_ago_label() renders the epoch (AGENTS.md: a
 * task once read `20694d ago` on every row).
 *
 * Returns: (transfer full): "asked by kudu · 5m ago"
 */
gchar *clawt_lrg_decision_meta(JsonObject *decision, gint64 now_us);

/**
 * clawt_lrg_decision_answer:
 * @option: (nullable): the option clicked
 * @typed: (nullable): what is in the answer box
 *
 * A typed answer wins over a clicked option, because somebody who typed
 * something meant it -- and "neither, do X" is the most valuable answer
 * there is.
 *
 * Returns: (transfer none) (nullable): the answer, or %NULL for none
 */
const gchar *clawt_lrg_decision_answer(const gchar *option,
                                       const gchar *typed);

/* ── Go to… ───────────────────────────────────────────────────────── */

/**
 * ClawtLrgNavKind:
 * @CLAWT_LRG_NAV_AGENT: an agent
 * @CLAWT_LRG_NAV_ROOM: a declared room
 * @CLAWT_LRG_NAV_PAGE: a page of the window
 *
 * What a destination is.
 */
typedef enum {
    CLAWT_LRG_NAV_AGENT = 0,
    CLAWT_LRG_NAV_ROOM,
    CLAWT_LRG_NAV_PAGE
} ClawtLrgNavKind;

/**
 * ClawtLrgNavTarget:
 * @kind: what it is
 * @name: what the row says
 * @detail: the line under it
 * @target: the agent id, room id or page nick
 * @search: what the query is matched against
 *
 * One destination.  A snapshot: it owns copies of everything, because
 * a fleet refresh can replace the listing while the palette is open.
 */
typedef struct {
    ClawtLrgNavKind  kind;
    gchar           *name;
    gchar           *detail;
    gchar           *target;
    gchar           *search;
} ClawtLrgNavTarget;

/**
 * clawt_lrg_nav_targets:
 * @agents: (nullable): an `agent.list` reply
 * @rooms: (nullable): a `room.list` reply
 * @agent_selected: whether an agent is selected
 *
 * Every destination, agents then declared rooms then pages.  The pages
 * are walked from the library's sections; with no agent selected only
 * the pages that do not need one are offered (Memory), as GTK does.
 *
 * Returns: (transfer full) (element-type ClawtLrgNavTarget): them
 */
GPtrArray *clawt_lrg_nav_targets(JsonNode *agents,
                                 JsonNode *rooms,
                                 gboolean  agent_selected);

/**
 * clawt_lrg_nav_filter:
 * @targets: (element-type ClawtLrgNavTarget): destinations
 * @query: (nullable): what was typed
 *
 * Filtered with clawt_navigation_matches(), the rule both other clients
 * use, so a search finds the same things everywhere.
 *
 * Returns: (transfer full) (element-type guint): indices into @targets
 */
GArray *clawt_lrg_nav_filter(GPtrArray *targets, const gchar *query);

/**
 * clawt_lrg_nav_move:
 * @cursor: the highlighted result
 * @delta: -1 for up, +1 for down
 * @n: how many results there are
 *
 * Returns: the new cursor, clamped (0 when there are none)
 */
guint clawt_lrg_nav_move(guint cursor, gint delta, guint n);

G_END_DECLS
