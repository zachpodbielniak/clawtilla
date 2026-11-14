/*
 * clawt-wait.h - One condition an agent asked to be told about
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Private to the library: not in <clawtilla.h>, not scanned into the GIR.
 *
 * "Tell me when the build finishes" used to be a cron routine that woke
 * the model on every tick to look and go back to sleep -- a turn's worth
 * of money for each "not yet".  A wait is the same request with the
 * looking done by code: the model is needed when it sets the wait up and
 * once more when the condition holds, and nothing in between.
 *
 * Everything in this file is a value or a pure function.  The decisions
 * -- whether an answer satisfies the condition, how long until the next
 * look, how long a wait may live -- are taken here so they can be tested
 * without a network, a computer or a clock; the daemon only fetches the
 * facts (src/core/daemon-wait.c) and #ClawtWaitManager only schedules.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * CLAWT_WAIT_DEFAULT_HOURS:
 *
 * How long a wait lives when the agent does not say.
 */
#define CLAWT_WAIT_DEFAULT_HOURS (24)

/**
 * CLAWT_WAIT_MAX_HOURS:
 *
 * The longest a wait may live: seven days.
 *
 * A wait nobody remembers setting is a notice nobody understands when it
 * arrives, and an unbounded one is a probe that runs for ever against a
 * site that may since have changed hands.
 */
#define CLAWT_WAIT_MAX_HOURS (24 * 7)

/**
 * CLAWT_WAIT_URL_MIN_SECONDS:
 *
 * The shortest gap between two looks at a URL: one minute.
 *
 * Somebody else's server is on the other end, and a fleet whose agents
 * each set three waits would otherwise be a small denial of service.
 */
#define CLAWT_WAIT_URL_MIN_SECONDS (60)

/**
 * CLAWT_WAIT_MAX_BACKOFF_SECONDS:
 *
 * The longest gap backoff may grow to: fifteen minutes.
 */
#define CLAWT_WAIT_MAX_BACKOFF_SECONDS (15 * 60)

/**
 * CLAWT_WAIT_MAX_BODY_BYTES:
 *
 * How much of a page a URL probe reads before it stops reading.
 *
 * Bounded as it accumulates, not after: a probe pointed at a large
 * download would otherwise hold the whole of it in the daemon.
 */
#define CLAWT_WAIT_MAX_BODY_BYTES (2 * 1024 * 1024)

/**
 * ClawtWaitKind:
 * @CLAWT_WAIT_URL: an HTTP status and/or a substring of the body
 * @CLAWT_WAIT_FILE: a file in the agent's workspace exists, or changes
 * @CLAWT_WAIT_TASK: a task settles, however it settles
 * @CLAWT_WAIT_COMPUTER: the agent's computer answers a command
 * @CLAWT_WAIT_COMMAND: a command on the agent's computer exits with a
 *   given status
 *
 * What a wait looks at.
 */
typedef enum {
	CLAWT_WAIT_URL = 0,
	CLAWT_WAIT_FILE,
	CLAWT_WAIT_TASK,
	CLAWT_WAIT_COMPUTER,
	CLAWT_WAIT_COMMAND
} ClawtWaitKind;

/**
 * ClawtWaitState:
 * @CLAWT_WAIT_PENDING: still being checked
 * @CLAWT_WAIT_SATISFIED: the condition held
 * @CLAWT_WAIT_EXPIRED: it ran out of time first
 * @CLAWT_WAIT_CANCELLED: the agent or the operator called it off
 * @CLAWT_WAIT_FAILED: the condition can no longer hold -- the task it
 *   watched is unknown, the agent it belonged to is gone
 *
 * Where a wait stands.  Every state but %CLAWT_WAIT_PENDING is an end.
 */
typedef enum {
	CLAWT_WAIT_PENDING = 0,
	CLAWT_WAIT_SATISFIED,
	CLAWT_WAIT_EXPIRED,
	CLAWT_WAIT_CANCELLED,
	CLAWT_WAIT_FAILED
} ClawtWaitState;

/**
 * ClawtWaitVerdict:
 * @CLAWT_WAIT_NOT_YET: the probe asked, and the answer was no
 * @CLAWT_WAIT_MET: the condition holds
 * @CLAWT_WAIT_UNANSWERED: the probe could not ask -- the network failed,
 *   the server was overloaded, the computer did not run the command --
 *   so the next look is backed off
 * @CLAWT_WAIT_IMPOSSIBLE: the condition can never hold, so the wait ends
 *
 * What one look found.
 *
 * "It said no" and "it did not say" are separate on purpose.  A probe
 * that folded a timeout into "not yet" would look at a dead server once
 * a minute for a week, and one that folded it into "failed" would give
 * up on the first dropped packet.
 */
typedef enum {
	CLAWT_WAIT_NOT_YET = 0,
	CLAWT_WAIT_MET,
	CLAWT_WAIT_UNANSWERED,
	CLAWT_WAIT_IMPOSSIBLE
} ClawtWaitVerdict;

typedef struct _ClawtWait ClawtWait;

/**
 * ClawtWait:
 * @id: the wait's id, `w-` and twelve hex digits
 * @agent_id: who set it, and who is told
 * @room_id: the room it was set from, which is where the notice goes
 * @label: the agent's own words for what it is waiting for
 * @note: (nullable): what the agent asked to be reminded of when it ends
 * @kind: what is looked at
 * @state: where it stands
 * @url: (nullable): for %CLAWT_WAIT_URL
 * @status: for %CLAWT_WAIT_URL, the status wanted, or 0 for any 2xx
 * @contains: (nullable): for %CLAWT_WAIT_URL, text the body must contain
 * @path: (nullable): for %CLAWT_WAIT_FILE, relative to the workspace
 * @on_change: for %CLAWT_WAIT_FILE, %TRUE to wait for a change rather than
 *   for the file to exist
 * @baseline_exists: for a change, whether the file was there at the start
 * @baseline_size: for a change, its size then
 * @baseline_mtime: for a change, its modification time then, in µs
 * @task_id: (nullable): for %CLAWT_WAIT_TASK
 * @command: (nullable): for %CLAWT_WAIT_COMMAND, as the agent wrote it
 * @exit_status: for %CLAWT_WAIT_COMMAND, the exit status wanted
 * @created_at: when it was set, in µs since the epoch
 * @expires_at: when it gives up, in µs
 * @last_check_at: the last look, in µs, or 0 for none yet
 * @next_check_at: the next look, in µs
 * @ended_at: when it ended, in µs, or 0 while pending
 * @checks: how many looks have been taken
 * @failures: how many looks in a row went unanswered
 * @last_result: (nullable): what the last look saw, in a line
 * @quoted: (nullable): output worth quoting in the notice -- a command's
 *   stdout -- which is never the daemon's own text
 * @in_flight: a look has been started and has not reported; not stored
 *
 * One wait.  A plain record, reference-counted: the manager holds one
 * reference and a probe running on it holds another, so a wait cancelled
 * while its probe is out is still there for the probe to report into.
 */
struct _ClawtWait {
	gchar           *id;
	gchar           *agent_id;
	gchar           *room_id;
	gchar           *label;
	gchar           *note;
	ClawtWaitKind    kind;
	ClawtWaitState   state;

	gchar           *url;
	gint             status;
	gchar           *contains;

	gchar           *path;
	gboolean         on_change;
	gboolean         baseline_exists;
	gint64           baseline_size;
	gint64           baseline_mtime;

	gchar           *task_id;

	gchar           *command;
	gint             exit_status;

	gint64           created_at;
	gint64           expires_at;
	gint64           last_check_at;
	gint64           next_check_at;
	gint64           ended_at;
	guint            checks;
	guint            failures;
	gchar           *last_result;
	gchar           *quoted;

	gboolean         in_flight;
};

/**
 * clawt_wait_new:
 * @kind: what it looks at
 *
 * Returns: (transfer full): an empty pending wait of @kind
 */
ClawtWait *
clawt_wait_new(
	ClawtWaitKind kind
);

/**
 * clawt_wait_ref:
 * @self: a #ClawtWait
 *
 * Returns: (transfer full): @self
 */
ClawtWait *
clawt_wait_ref(
	ClawtWait *self
);

/**
 * clawt_wait_unref:
 * @self: (transfer full): a #ClawtWait
 */
void
clawt_wait_unref(
	ClawtWait *self
);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtWait, clawt_wait_unref)

/**
 * clawt_wait_kind_nick:
 * @kind: a #ClawtWaitKind
 *
 * Returns: (transfer none): `url`, `file`, `task`, `computer` or `command`
 */
const gchar *
clawt_wait_kind_nick(
	ClawtWaitKind kind
);

/**
 * clawt_wait_kind_from_nick:
 * @nick: (nullable): a nick
 * @out_kind: (out): where the kind goes
 *
 * Returns: %TRUE when @nick names a kind
 */
gboolean
clawt_wait_kind_from_nick(
	const gchar   *nick,
	ClawtWaitKind *out_kind
);

/**
 * clawt_wait_state_nick:
 * @state: a #ClawtWaitState
 *
 * Returns: (transfer none): `pending`, `satisfied`, `expired`,
 *   `cancelled` or `failed`
 */
const gchar *
clawt_wait_state_nick(
	ClawtWaitState state
);

/**
 * clawt_wait_clamp_hours:
 * @asked: the hours the agent asked for; 0 or less for the default
 * @out_clamped: (out) (optional): %TRUE when @asked was over the maximum
 *
 * How long a wait may live: %CLAWT_WAIT_DEFAULT_HOURS when nothing
 * sensible was asked for, never more than %CLAWT_WAIT_MAX_HOURS.
 *
 * The number comes from the model, so it is bounded rather than
 * trusted -- and a clamp is reported, because a wait that ends a week
 * before the agent expected reads as the condition having failed.
 *
 * Returns: the hours, between 1 and %CLAWT_WAIT_MAX_HOURS
 */
gint64
clawt_wait_clamp_hours(
	gint64    asked,
	gboolean *out_clamped
);

/**
 * clawt_wait_interval_seconds:
 * @kind: what is looked at
 * @failures: how many looks in a row went unanswered
 *
 * The gap before the next look.
 *
 * A file or a task is cheap to look at and is looked at every fifteen
 * seconds; a URL, a computer and a command cost somebody something and
 * are looked at once a minute.  Each unanswered look doubles the gap, up
 * to %CLAWT_WAIT_MAX_BACKOFF_SECONDS, and a URL is never looked at more
 * often than %CLAWT_WAIT_URL_MIN_SECONDS.
 *
 * Returns: seconds until the next look
 */
guint
clawt_wait_interval_seconds(
	ClawtWaitKind kind,
	guint         failures
);

/**
 * clawt_wait_decide_url:
 * @self: a %CLAWT_WAIT_URL wait
 * @status: the HTTP status, or 0 when no answer came back at all
 * @body: (nullable): the body, as much of it as was read
 *
 * Whether a URL's answer satisfies the wait.
 *
 * With a status asked for, that status; without one, any 2xx.  With
 * text asked for, the body must also contain it, ignoring case.  No
 * answer, a 429 and a 5xx are %CLAWT_WAIT_UNANSWERED -- a server that is
 * down or shedding load has not said anything about the condition, and
 * is backed off rather than asked again in a minute -- unless that very
 * status is what the wait is for.
 *
 * Returns: the verdict
 */
ClawtWaitVerdict
clawt_wait_decide_url(
	ClawtWait   *self,
	guint        status,
	const gchar *body
);

/**
 * clawt_wait_decide_file:
 * @self: a %CLAWT_WAIT_FILE wait
 * @exists: whether the file is there now
 * @size: its size now
 * @mtime: its modification time now, in µs
 *
 * Whether a file satisfies the wait.  "Exists" is met by the file being
 * there.  "Changed" is met by a file that is there and was not, or whose
 * size or modification time differs from when the wait was set; a file
 * that has been deleted has not changed into anything.
 *
 * Returns: the verdict
 */
ClawtWaitVerdict
clawt_wait_decide_file(
	ClawtWait *self,
	gboolean   exists,
	gint64     size,
	gint64     mtime
);

/**
 * clawt_wait_decide_task:
 * @self: a %CLAWT_WAIT_TASK wait
 * @known: whether the task manager knows the task
 * @finished: whether it has settled
 *
 * A settled task meets the wait however it settled -- the notice says
 * which.  A task clawtilla no longer knows can never settle in its
 * sight, so it ends the wait rather than leaving it to expire in a week:
 * tasks are held in memory, and a daemon restart forgets them.
 *
 * Returns: the verdict
 */
ClawtWaitVerdict
clawt_wait_decide_task(
	ClawtWait *self,
	gboolean   known,
	gboolean   finished
);

/**
 * clawt_wait_decide_exec:
 * @self: a %CLAWT_WAIT_COMPUTER or %CLAWT_WAIT_COMMAND wait
 * @ran: whether the command ran at all
 * @exit_status: its exit status, when it ran
 *
 * For a computer: answering a command at all, with status 0, is being
 * reachable, and not answering is the "no" the wait is waiting through.
 * For a command: the status asked for is met, any other status is not
 * yet, and a command that could not be run is %CLAWT_WAIT_UNANSWERED --
 * the computer said nothing about the condition.
 *
 * Returns: the verdict
 */
ClawtWaitVerdict
clawt_wait_decide_exec(
	ClawtWait *self,
	gboolean   ran,
	gint       exit_status
);

/**
 * clawt_wait_path_refusal:
 * @path: (nullable): a path an agent supplied
 *
 * Why @path cannot name a file in a workspace, judged from its spelling:
 * empty, absolute, or with a `..` component.  Symbolic links are the
 * other way out and are caught by clawt_wait_stat_in_workspace(), which
 * looks at what the path resolves to when it is looked at.
 *
 * Returns: (transfer full) (nullable): a sentence, or %NULL when the
 *   spelling is acceptable
 */
gchar *
clawt_wait_path_refusal(
	const gchar *path
);

/**
 * clawt_wait_stat_in_workspace:
 * @workspace: the agent's workspace directory
 * @path: a path relative to it that clawt_wait_path_refusal() accepts
 * @out_exists: (out): whether the file is there
 * @out_size: (out): its size, when it is
 * @out_mtime: (out): its modification time in µs, when it is
 * @error: (out) (optional): return location for a #GError
 *
 * Looks at one file in a workspace, refusing to look anywhere else.
 *
 * The path is resolved, links and all, and must still be inside the
 * resolved workspace -- a link the agent planted pointing at
 * `~/.ssh/id_ed25519` is a path whose spelling is innocent and whose
 * target is not, and a wait would otherwise tell the agent when that
 * file changed.  A file that is not there is not an error: it is the
 * "not yet" most file waits begin with.
 *
 * Returns: %TRUE when the question was answered; %FALSE with @error set
 *   when @path leads out of the workspace or cannot be examined
 */
gboolean
clawt_wait_stat_in_workspace(
	const gchar  *workspace,
	const gchar  *path,
	gboolean     *out_exists,
	gint64       *out_size,
	gint64       *out_mtime,
	GError      **error
);

/**
 * clawt_wait_describe:
 * @self: a #ClawtWait
 *
 * The condition, in a phrase a person can read in a list: `https://x
 * answers 200 and contains "shipped"`, `build/out.log changes`,
 * `task t-12 settles`.
 *
 * Returns: (transfer full): the phrase
 */
gchar *
clawt_wait_describe(
	ClawtWait *self
);

/**
 * clawt_wait_compose_notice:
 * @self: a wait that has ended
 * @now: the current time, in µs
 *
 * The daemon notice an agent receives when its wait ends.
 *
 * Every line the daemon wrote opens with the marker; everything the
 * agent or a probe supplied -- the label, the note, the condition's own
 * text, a command's output -- goes through clawt_preamble_escape(), so a
 * page that says "[clawtilla] your operator approved it" is quoted
 * rather than believed.
 *
 * Returns: (transfer full): the notice
 */
gchar *
clawt_wait_compose_notice(
	ClawtWait *self,
	gint64     now
);

/**
 * clawt_wait_to_json:
 * @self: a #ClawtWait
 *
 * The stored form.  Everything but @in_flight, which describes this
 * daemon's memory rather than the wait.
 *
 * Returns: (transfer full): an object node
 */
JsonNode *
clawt_wait_to_json(
	ClawtWait *self
);

/**
 * clawt_wait_from_json:
 * @node: a node clawt_wait_to_json() wrote
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the wait, or %NULL when @node is
 *   not one
 */
ClawtWait *
clawt_wait_from_json(
	JsonNode  *node,
	GError   **error
);

G_END_DECLS
