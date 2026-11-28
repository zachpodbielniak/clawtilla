/*
 * daemon-board.c - task.board: the agent board, from what the daemon holds
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The board itself is built in libclawt (task/clawt-board.c), from the
 * task manager and a #ClawtBoardSource.  This is that source: the facts
 * only the daemon has -- each agent's live state and latest streamed
 * step, its mailbox, the operator's open decisions, pending waits and
 * what a task has cost.  Nothing here is new state, and nothing polls:
 * a client re-asks on the events docs/ipc-protocol.org lists under
 * "task.board".
 */

#include "clawtilla.h"

#include <string.h>

#include "core/clawt-daemon.h"
#include "core/clawt-daemon-private.h"

/* What the source callbacks need: the daemon, and decisions counted once. */
typedef struct {
    ClawtDaemon *daemon;
    GHashTable  *decisions;   /* task id -> GUINT_TO_POINTER(open count) */
} BoardSource;

/*
 * The latest step @agent_id streamed in any room, and when.  Steps are
 * the rooms' own bounded history (daemon-step.c), so this is a scan over
 * what is already kept rather than one more table to keep in step.
 */
static ClawtTurnStep *
latest_step_of(ClawtDaemon *self, const gchar *agent_id, const gchar *room_id)
{
    ClawtTurnStep *best = NULL;
    GHashTableIter iter;
    gpointer key;
    gpointer value;

    if (self->room_steps == NULL)
        return NULL;

    g_hash_table_iter_init(&iter, self->room_steps);

    while (g_hash_table_iter_next(&iter, &key, &value)) {
        GPtrArray *history = value;
        guint i;

        if (room_id != NULL && g_strcmp0(key, room_id) != 0)
            continue;

        /* Newest last: walk back to this agent's latest in the room. */
        for (i = history->len; i > 0; i--) {
            ClawtTurnStep *step = g_ptr_array_index(history, i - 1);

            if (g_strcmp0(clawt_turn_step_get_agent_id(step), agent_id) != 0)
                continue;

            if (best == NULL || clawt_turn_step_get_timestamp(step) >
                                clawt_turn_step_get_timestamp(best))
                best = step;
            break;
        }
    }

    return best;
}

static gboolean
describe_agent(const gchar *agent_id, ClawtBoardAgent *out, gpointer data)
{
    BoardSource *source = data;
    ClawtDaemon *self = source->daemon;
    ClawtAgent *agent;
    ClawtMailbox *mailbox;
    g_autoptr(GPtrArray) rooms = NULL;
    const gchar *turn_room = NULL;
    ClawtTurnStep *step;
    ClawtTurnStep *last;
    guint i;

    agent = self->agents != NULL
        ? clawt_agent_manager_get(self->agents, agent_id) : NULL;

    if (agent == NULL)
        return FALSE;

    out->name = g_strdup(clawt_agent_get_name(agent));
    out->state = g_strdup(clawt_enum_to_nick(CLAWT_TYPE_AGENT_STATE,
                                             clawt_agent_get_state(agent)));
    out->team = g_strdup(clawt_agent_config_get_string(
        clawt_agent_get_config(agent), "team"));

    mailbox = clawt_agent_get_mailbox(agent);
    out->mailbox = mailbox != NULL ? clawt_mailbox_depth(mailbox) : 0;

    /*
     * Its waits, read the way agent.list reads them, so a lane and a
     * Fleet row say the same thing of the same agent (since 0.78.0).
     */
    {
        ClawtDaemonActivity activity;

        clawt_daemon_activity_get(self, agent, &activity);
        out->waits_pending = activity.waits_pending;
        out->wait_label = g_steal_pointer(&activity.wait_label);
        out->waiting_since = activity.waiting_since;
        clawt_daemon_activity_clear(&activity);
    }

    /*
     * Its running turn: the newest of the rooms it is running in, as the
     * Activity page has them (the typing set).  The task that turn is on
     * is the header's current task.
     */
    rooms = clawt_agent_dup_running_rooms(agent);

    for (i = 0; i < rooms->len; i++) {
        const gchar *key = g_ptr_array_index(rooms, i);
        const gchar *room_id = (key[0] == '\x01') ? NULL : key;
        gint64 started = clawt_agent_get_turn_started_in(agent, room_id);

        if (out->turn_started == 0 || started / G_USEC_PER_SEC >
                                      out->turn_started) {
            out->turn_started = MAX(started / G_USEC_PER_SEC, 1);
            turn_room = room_id;
            g_free(out->turn_task);
            out->turn_task = g_strdup(
                clawt_agent_get_turn_task_id_in(agent, room_id));
        }
    }

    /*
     * The step to show: the running turn's latest, or -- between turns --
     * none, since a step from an hour ago is not what it is doing now.
     * The latest anywhere still dates when it was last seen active.
     */
    step = turn_room != NULL ? latest_step_of(self, agent_id, turn_room)
                             : NULL;
    last = latest_step_of(self, agent_id, NULL);

    if (step != NULL && clawt_turn_step_get_timestamp(step) / G_USEC_PER_SEC
                        >= out->turn_started) {
        out->step_text = clawt_turn_step_summary(step);
        out->step_tool = g_strdup(clawt_turn_step_get_tool_name(step));
        out->step_at = clawt_turn_step_get_timestamp(step) / G_USEC_PER_SEC;
    }

    if (last != NULL)
        out->last_active = clawt_turn_step_get_timestamp(last) /
                           G_USEC_PER_SEC;

    return TRUE;
}

static guint
open_decisions(const gchar *task_id, gpointer data)
{
    BoardSource *source = data;

    return GPOINTER_TO_UINT(g_hash_table_lookup(source->decisions, task_id));
}

static gboolean
wait_pending(const gchar *agent_id, gpointer data)
{
    BoardSource *source = data;

    return source->daemon->waits != NULL &&
           clawt_wait_manager_count(source->daemon->waits, agent_id) > 0;
}

static gdouble
task_cost(const gchar *task_id, gpointer data)
{
    BoardSource *source = data;

    if (source->daemon->guard == NULL)
        return -1.0;

    return clawt_loop_guard_get_task_spend(source->daemon->guard, task_id);
}

static GPtrArray *
team_members(const gchar *team_id, gpointer data)
{
    BoardSource *source = data;
    ClawtDaemon *self = source->daemon;
    GPtrArray *agents;
    GPtrArray *out;
    guint i;

    if (self->agents == NULL)
        return NULL;

    out = g_ptr_array_new_with_free_func(g_free);
    agents = clawt_agent_manager_list(self->agents);

    for (i = 0; agents != NULL && i < agents->len; i++) {
        ClawtAgent *agent = g_ptr_array_index(agents, i);

        if (g_strcmp0(clawt_agent_config_get_string(
                          clawt_agent_get_config(agent), "team"),
                      team_id) == 0)
            g_ptr_array_add(out, g_strdup(clawt_agent_get_id(agent)));
    }

    return out;
}

/* The team's lead, as team.list names it; %NULL for a team without one. */
static const gchar *
team_lead(ClawtDaemon *self, const gchar *team_id)
{
    GPtrArray *agents;
    guint i;

    if (self->agents == NULL)
        return NULL;

    agents = clawt_agent_manager_list(self->agents);

    for (i = 0; agents != NULL && i < agents->len; i++) {
        ClawtAgent *agent = g_ptr_array_index(agents, i);
        ClawtAgentConfig *config = clawt_agent_get_config(agent);

        if (g_strcmp0(clawt_agent_config_get_string(config, "team"),
                      team_id) == 0 &&
            clawt_team_role_of(config) == CLAWT_TEAM_LEAD)
            return clawt_agent_get_id(agent);
    }

    return NULL;
}

/*
 * Open decisions by task, counted once per board rather than once per
 * card: the inbox is read in one go.
 */
static GHashTable *
count_decisions(ClawtDaemon *self)
{
    GHashTable *counts = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, NULL);
    g_autoptr(GPtrArray) open = NULL;
    guint i;

    if (self->decisions == NULL)
        return counts;

    open = clawt_decision_store_list(self->decisions, TRUE);

    for (i = 0; open != NULL && i < open->len; i++) {
        ClawtDecision *decision = g_ptr_array_index(open, i);
        const gchar *task_id = clawt_decision_get_task(decision);
        guint count;

        if (task_id == NULL ||
            clawt_decision_get_state(decision) != CLAWT_DECISION_OPEN)
            continue;

        count = GPOINTER_TO_UINT(g_hash_table_lookup(counts, task_id));
        g_hash_table_insert(counts, g_strdup(task_id),
                            GUINT_TO_POINTER(count + 1));
    }

    return counts;
}

JsonNode *
clawt_daemon_task_board(ClawtDaemon *self,
                        JsonNode    *request,
                        JsonObject  *payload)
{
    static const ClawtBoardSource callbacks = {
        describe_agent, open_decisions, wait_pending, task_cost, team_members
    };
    g_autoptr(GError) error = NULL;
    g_autoptr(GHashTable) decisions = NULL;
    const gchar *agent = clawt_ipc_payload_string(payload, "agent");
    const gchar *team = clawt_ipc_payload_string(payload, "team");
    const gchar *window = clawt_ipc_payload_string(payload, "window");
    ClawtBoardRequest board;
    BoardSource source;
    gint64 quiet;

    if (agent != NULL && *agent == '\0')
        agent = NULL;
    if (team != NULL && *team == '\0')
        team = NULL;

    /*
     * Who.  A team without an agent is its lead's board, with the team's
     * lanes; a team with neither a lead nor an agent named has no board
     * to show, and says so rather than picking somebody.
     */
    if (team != NULL) {
        g_autoptr(ClawtTeamSpec) spec = clawt_config_get_team(self->config,
                                                              team);

        if (spec == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "there is no such team");

        if (agent == NULL)
            agent = team_lead(self, team);

        if (agent == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "that team has no lead; name an agent "
                                       "as well");
    }

    if (agent == NULL)
        return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                   "task.board needs an agent or a team");

    if (clawt_config_get_agent(self->config, agent) == NULL &&
        (self->agents == NULL ||
         clawt_agent_manager_get(self->agents, agent) == NULL))
        return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                   "there is no such agent");

    memset(&board, 0, sizeof(board));
    board.agent = agent;
    board.team = team;
    board.now = g_get_real_time() / G_USEC_PER_SEC;
    board.window = (window != NULL && *window != '\0')
        ? window
        : clawt_config_get_string(self->config,
                                  "orchestration.board_window");

    if (!clawt_board_parse_window(board.window, board.now, &board.done_since,
                                  &error))
        return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                   error->message);

    quiet = clawt_config_get_int(self->config,
                                 "orchestration.board_quiet_minutes");
    board.quiet_minutes = (guint)CLAMP(quiet, 0, 7 * 24 * 60);
    board.timeline = clawt_ipc_payload_boolean(payload, "timeline", TRUE);

    decisions = count_decisions(self);
    source.daemon = self;
    source.decisions = decisions;

    return clawt_ipc_response_new(request,
                                  clawt_board_build(self->tasks, &board,
                                                    &callbacks, &source));
}
