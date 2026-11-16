/*
 * clawt-venture-mirror.h - Tasks copied into VENTURE tickets
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The mirror observes; it never steers.  The task store writes an
 * outbox row in the same transaction as every change it records (see
 * clawt_task_store_set_mirror()), and this object drains that outbox on
 * a timer: nothing an agent does ever waits on VENTURE, and a VENTURE
 * that is down only means the outbox grows until it is back.
 *
 * Every write is made as the agent it is about, with that agent's own
 * connector token: the delegator's account files the ticket, the
 * assignee's keeps it current, a note's author writes its comment.  An
 * account is used only once `GET /api/v1/whoami` has said who it is, so
 * the ticket's assignee is the assignee's real VENTURE username and not
 * a guess from its agent id.
 *
 * A task is mirrored only when its repository is one VENTURE knows: the
 * project's pinned repository, or a checkout remote that
 * `/api/v1/forge_repo/resolve` answers for that agent's token.  No
 * match, no ticket -- and the task's ref says why.
 *
 * The transport is a function the owner installs, as the confirmation
 * bridge's is, so every rule here runs against a fixture with no socket.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "task/clawt-task-store.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_VENTURE_MIRROR (clawt_venture_mirror_get_type())

G_DECLARE_FINAL_TYPE(ClawtVentureMirror, clawt_venture_mirror,
                     CLAWT, VENTURE_MIRROR, GObject)

/**
 * ClawtVentureMirrorReply:
 * @status: the HTTP status, or 0 when nothing came back (network)
 * @body: (nullable): the answer's body
 * @reply_data: what the mirror passed with the request
 *
 * How a request func hands its answer back.  Called exactly once per
 * request, from the mirror's context.
 */
typedef void (*ClawtVentureMirrorReply)(guint        status,
                                        const gchar *body,
                                        gpointer     reply_data);

/**
 * ClawtVentureMirrorRequestFunc:
 * @mirror: the mirror
 * @method: `GET`, `POST` or `PATCH`
 * @url: the full URL
 * @token: the acting agent's bearer token -- live credential material;
 *   never logged or stored
 * @idempotency_key: (nullable): an `Idempotency-Key` header to send
 * @body: (nullable): a JSON body
 * @reply: where the answer goes
 * @reply_data: pass to @reply
 * @user_data: data passed to clawt_venture_mirror_set_request_func()
 *
 * Sends one request.  It must not block.
 */
typedef void (*ClawtVentureMirrorRequestFunc)(ClawtVentureMirror     *mirror,
                                              const gchar            *method,
                                              const gchar            *url,
                                              const gchar            *token,
                                              const gchar            *idempotency_key,
                                              const gchar            *body,
                                              ClawtVentureMirrorReply reply,
                                              gpointer                reply_data,
                                              gpointer                user_data);

/**
 * ClawtVentureMirrorTargetFunc:
 * @mirror: the mirror
 * @task_id: the task
 * @repo_id: (out): a pinned VENTURE `forge_repo` id, or 0
 * @user_data: data passed to clawt_venture_mirror_set_target_func()
 *
 * Where a task's work is: the project's pinned repository, and/or the
 * fetch URLs of its checkout's (or project directory's) git remotes,
 * origin first.
 *
 * Returns: (transfer full) (nullable): the remote URLs, or %NULL
 */
typedef GStrv (*ClawtVentureMirrorTargetFunc)(ClawtVentureMirror *mirror,
                                              const gchar        *task_id,
                                              gint64             *repo_id,
                                              gpointer            user_data);

/**
 * ClawtVentureMirrorInbound: (skip)
 * @cancel: a person cancelled the task's ticket: cancel the task
 * @review: the ticket left review -- @accept for `done`, otherwise
 *   changes were asked for, with @notes
 * @comment: somebody other than the mirror and the assignee commented
 * @intake: a ticket assigned to @agent_id's VENTURE user, still `todo`
 *   and from no system: start a task for it, and return its id (or
 *   %NULL when it was refused).  @from_agent is the fleet agent whose
 *   own VENTURE account assigned it, or %NULL for a person (since
 *   0.47.0): an agent handing another work through VENTURE is a
 *   delegation, held to the same loop limits as one through clawtilla
 *
 * What the mirror asks the daemon to do with changes made in VENTURE
 * (since 0.45.0).  @who is the VENTURE actor or author, as VENTURE spelt
 * it; @title, @description and @text are people's text -- data, never
 * instructions -- and the daemon fences them before an agent reads them.
 */
typedef struct {
    void      (*cancel)(gpointer data, const gchar *task_id, const gchar *who);
    void      (*review)(gpointer data, const gchar *task_id, gboolean accept,
                        const gchar *notes, const gchar *who);
    void      (*comment)(gpointer data, const gchar *task_id, const gchar *who,
                         const gchar *text);
    gchar    *(*intake)(gpointer data, const gchar *agent_id, gint64 ticket,
                        const gchar *title, const gchar *description,
                        const gchar *url, const gchar *from_agent);
} ClawtVentureMirrorInbound;

/**
 * clawt_venture_mirror_set_inbound: (skip)
 * @self: a #ClawtVentureMirror
 * @inbound: (nullable): what to do with changes made in VENTURE; copied
 * @data: passed to each of @inbound's functions
 * @intake: whether tickets assigned to an agent start tasks
 *
 * Turns the reverse direction on: a poll every minute reads the
 * mirrored tickets that changed, the comments on those still going and,
 * with @intake, each account's new tickets.  %NULL turns it off.
 */
void clawt_venture_mirror_set_inbound(ClawtVentureMirror              *self,
                                      const ClawtVentureMirrorInbound *inbound,
                                      gpointer                         data,
                                      gboolean                         intake);

/**
 * clawt_venture_mirror_poll:
 * @self: a #ClawtVentureMirror
 *
 * Reads VENTURE now, if nothing is in flight -- the timer's work, and a
 * webhook's wake-up.
 */
void clawt_venture_mirror_poll(ClawtVentureMirror *self);

/**
 * ClawtVentureMirrorCostFunc:
 * @mirror: the mirror
 * @task_id: a finished task
 * @user_data: data passed to clawt_venture_mirror_set_cost_func()
 *
 * What a task cost, every agent's turns on it together.
 *
 * Returns: micro-dollars
 */
typedef gint64 (*ClawtVentureMirrorCostFunc)(ClawtVentureMirror *mirror,
                                             const gchar        *task_id,
                                             gpointer            user_data);

/**
 * clawt_venture_mirror_set_cost_func:
 * @self: a #ClawtVentureMirror
 * @func: (scope notified) (nullable): what a task cost
 * @user_data: data for @func
 * @notify: (nullable): frees @user_data
 *
 * With one, a mirrored task that ran is recorded in VENTURE as an
 * external run (since 0.45.0): opened when it starts, closed with its
 * state, cost and branch when it finishes -- so VENTURE's agent budgets,
 * Runs page and delivery report count the fleet's work.
 */
void clawt_venture_mirror_set_cost_func(ClawtVentureMirror        *self,
                                        ClawtVentureMirrorCostFunc func,
                                        gpointer                   user_data,
                                        GDestroyNotify             notify);

/**
 * ClawtVentureMirrorContextFunc:
 * @mirror: the mirror
 * @task_id: a task about to be filed
 * @milestone_id: (out): its project's VENTURE milestone, or 0
 * @goal_id: (out): its project's VENTURE goal, or 0
 * @user_data: data passed to clawt_venture_mirror_set_context_func()
 *
 * The milestone and goal a new ticket is filed under (since 0.46.0).
 */
typedef void (*ClawtVentureMirrorContextFunc)(ClawtVentureMirror *mirror,
                                              const gchar        *task_id,
                                              gint64             *milestone_id,
                                              gint64             *goal_id,
                                              gpointer            user_data);

/**
 * clawt_venture_mirror_set_context_func:
 * @self: a #ClawtVentureMirror
 * @func: (scope notified) (nullable): a task's milestone and goal
 * @user_data: data for @func
 * @notify: (nullable): frees @user_data
 */
void clawt_venture_mirror_set_context_func(ClawtVentureMirror           *self,
                                           ClawtVentureMirrorContextFunc func,
                                           gpointer                      user_data,
                                           GDestroyNotify                notify);

/**
 * clawt_venture_mirror_set_context_ids:
 * @self: a #ClawtVentureMirror
 * @milestones: (array length=n_milestones) (nullable): milestone ids
 * @n_milestones: how many
 * @goals: (array length=n_goals) (nullable): goal ids
 * @n_goals: how many
 *
 * The milestones and goals whose name, due date and progress agents are
 * told about (since 0.46.0).  The reverse poll reads them once a day --
 * and at once when the set changes -- into ~tasks.db~, where
 * clawt_venture_mirror_context_line() reads them without the network.
 * ::context-changed says when what it read differs.
 */
void clawt_venture_mirror_set_context_ids(ClawtVentureMirror *self,
                                          const gint64       *milestones,
                                          guint               n_milestones,
                                          const gint64       *goals,
                                          guint               n_goals);

/**
 * clawt_venture_mirror_context_line:
 * @store: the task store
 * @milestone_id: a milestone, or 0
 * @goal_id: a goal, or 0
 *
 * One line about them, from what the poll last read: "milestone *Beta*,
 * due 2026-12-01 (open); goal *Ship 12 titles*: 4 of 12 titles".
 *
 * Returns: (transfer full) (nullable): the line, or %NULL when nothing
 *   has been read yet
 */
gchar *clawt_venture_mirror_context_line(ClawtTaskStore *store,
                                         gint64          milestone_id,
                                         gint64          goal_id);

/**
 * clawt_venture_mirror_find_pull_request:
 * @text: (nullable): what a task's assignee wrote -- its result
 * @number: (out) (optional): the pull request's number
 *
 * The first pull or merge request URL in @text, as GitHub
 * (`/pull/N`), Forgejo and Gitea (`/pulls/N`) and GitLab
 * (`/-/merge_requests/N`) spell them (since 0.48.0).  clawtilla opens
 * none itself: an agent that opened one says so, and this finds it.
 *
 * Returns: (transfer full) (nullable): the URL, or %NULL
 */
gchar *clawt_venture_mirror_find_pull_request(const gchar *text,
                                              gint64      *number);

/**
 * clawt_venture_mirror_new:
 * @store: the task store whose outbox is drained
 * @context: (nullable): the context the drain timer attaches to
 *
 * Returns: (transfer full): a mirror with no accounts, not yet running
 */
ClawtVentureMirror *clawt_venture_mirror_new(ClawtTaskStore *store,
                                             GMainContext   *context);

/**
 * clawt_venture_mirror_set_request_func:
 * @self: a #ClawtVentureMirror
 * @func: (scope notified) (nullable): how to send a request
 * @user_data: data for @func
 * @notify: (nullable): frees @user_data
 */
void clawt_venture_mirror_set_request_func(ClawtVentureMirror           *self,
                                           ClawtVentureMirrorRequestFunc func,
                                           gpointer                      user_data,
                                           GDestroyNotify                notify);

/**
 * clawt_venture_mirror_set_target_func:
 * @self: a #ClawtVentureMirror
 * @func: (scope notified) (nullable): where a task's work is
 * @user_data: data for @func
 * @notify: (nullable): frees @user_data
 */
void clawt_venture_mirror_set_target_func(ClawtVentureMirror          *self,
                                          ClawtVentureMirrorTargetFunc func,
                                          gpointer                     user_data,
                                          GDestroyNotify               notify);

/**
 * clawt_venture_mirror_set_account:
 * @self: a #ClawtVentureMirror
 * @agent_id: the agent
 * @instance: its connector's name
 * @base_url: that VENTURE, without a trailing slash
 * @token: its token
 *
 * Gives one agent its VENTURE account.  The account is asked who it is
 * before its first write.
 */
void clawt_venture_mirror_set_account(ClawtVentureMirror *self,
                                      const gchar        *agent_id,
                                      const gchar        *instance,
                                      const gchar        *base_url,
                                      const gchar        *token);

/**
 * clawt_venture_mirror_clear_accounts:
 * @self: a #ClawtVentureMirror
 */
void clawt_venture_mirror_clear_accounts(ClawtVentureMirror *self);

/**
 * clawt_venture_mirror_has_account:
 * @self: a #ClawtVentureMirror
 * @agent_id: an agent
 *
 * Returns: whether @agent_id has a mirroring account
 */
gboolean clawt_venture_mirror_has_account(ClawtVentureMirror *self,
                                          const gchar        *agent_id);

/**
 * clawt_venture_mirror_set_account_intake:
 * @self: a #ClawtVentureMirror
 * @agent_id: an agent with an account
 * @intake: whether tickets assigned to its VENTURE user start tasks
 */
void clawt_venture_mirror_set_account_intake(ClawtVentureMirror *self,
                                             const gchar        *agent_id,
                                             gboolean            intake);

/**
 * clawt_venture_mirror_account_count:
 * @self: a #ClawtVentureMirror
 *
 * Returns: how many agents have a mirroring account
 */
guint clawt_venture_mirror_account_count(ClawtVentureMirror *self);

/**
 * clawt_venture_mirror_account_user:
 * @self: a #ClawtVentureMirror
 * @agent_id: an agent
 *
 * Returns: (nullable): the VENTURE username its token belongs to, once
 *   whoami has answered
 */
const gchar *clawt_venture_mirror_account_user(ClawtVentureMirror *self,
                                               const gchar        *agent_id);

/**
 * clawt_venture_mirror_start:
 * @self: a #ClawtVentureMirror
 * @interval_seconds: how often the outbox is drained
 */
void clawt_venture_mirror_start(ClawtVentureMirror *self,
                                guint               interval_seconds);

/**
 * clawt_venture_mirror_stop:
 * @self: a #ClawtVentureMirror
 */
void clawt_venture_mirror_stop(ClawtVentureMirror *self);

/**
 * clawt_venture_mirror_kick:
 * @self: a #ClawtVentureMirror
 *
 * Drains now, if nothing is in flight -- after a task change, so a
 * ticket does not wait a whole interval.
 */
void clawt_venture_mirror_kick(ClawtVentureMirror *self);

/**
 * clawt_venture_mirror_is_busy:
 * @self: a #ClawtVentureMirror
 *
 * Returns: whether a request is in flight
 */
gboolean clawt_venture_mirror_is_busy(ClawtVentureMirror *self);

/**
 * clawt_venture_mirror_ticket_status:
 * @state: a #ClawtTaskState, as an int
 *
 * The VENTURE ticket status a task in @state is mirrored as:
 * unassigned `triage`, pending `todo`, running `in_progress`, blocked,
 * failed and stalled `blocked`, review `review`, completed `done`,
 * cancelled `cancelled`.
 *
 * Returns: (transfer none): the status
 */
const gchar *clawt_venture_mirror_ticket_status(gint state);

/**
 * clawt_venture_mirror_describe:
 * @store: the task store
 * @task_id: a task
 *
 * The task's mirror state for clients: an object with `state`
 * (`synced`, `queued`, `dead`, `skipped`), and as known `ticket`,
 * `url`, `reason` -- or %NULL when the mirror has never looked at it.
 *
 * Returns: (transfer full) (nullable): the object
 */
JsonNode *clawt_venture_mirror_describe(ClawtTaskStore *store,
                                        const gchar    *task_id);

G_END_DECLS
