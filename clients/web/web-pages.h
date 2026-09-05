/*
 * web-pages.h - Every view, and the routes that reach it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * One clawt_web_register_* per module, called from main().  A module that
 * is not registered serves nothing, and a view listed in ClawtPage
 * that no module renders is a tab leading to an empty page -- so the enum
 * and these functions are meant to be read together.
 */

#pragma once

#include "web-ui.h"

G_BEGIN_DECLS

/* ── The frame ───────────────────────────────────────────────────── */

/**
 * clawt_web_sidebar:
 * @app: a #ClawtWebApp
 * @selected: (nullable): the agent being looked at
 * @view: the current view, so the links keep it
 * @look: (nullable): what this browser asked for, or %NULL for the
 *   shipped defaults -- the sidebar reads whether to write each
 *   agent's description under its name
 *
 * The fleet, grouped by team, in the order the daemon returns.
 *
 * Grouping is emitted whenever the team changes rather than gathered
 * here, because `agent.list` returns the fleet already grouped and
 * already ordered.  Sorting it again would be a second answer to what
 * order the fleet is in, and the two would differ the first time
 * somebody reordered it.
 *
 * Returns: (transfer full): the sidebar
 */
HtmxElement *clawt_web_sidebar(ClawtWebApp        *app,
                               const gchar        *selected,
                               ClawtPage           view,
                               const ClawtWebLook *look);

/**
 * clawt_web_topbar:
 * @app: a #ClawtWebApp
 * @agent_id: (nullable): the agent being looked at
 * @view: which tab is current
 *
 * Returns: (transfer full): the title, the view tabs and the actions
 */
HtmxElement *clawt_web_topbar(ClawtWebApp  *app,
                              const gchar  *agent_id,
                              ClawtPage  view);

/* ── Views ───────────────────────────────────────────────────────── */

/**
 * clawt_web_view_body:
 * @app: a #ClawtWebApp
 * @agent_id: (nullable): the selected agent
 * @view: which view to build
 *
 * Dispatches to whichever module owns @view.
 *
 * One place decides this, so a link, a redirect and a refresh all reach
 * the same renderer.  Three call sites choosing for themselves is how a
 * view comes to look different depending on how you arrived at it.
 *
 * Returns: (transfer full): the view's content
 */
HtmxElement *clawt_web_view_body(ClawtWebApp  *app,
                                 const gchar  *agent_id,
                                 ClawtPage  view);

HtmxElement *clawt_web_chat_body(ClawtWebApp *app, const gchar *agent_id);

/**
 * clawt_web_chat_body_full:
 * @app: a #ClawtWebApp
 * @agent_id: (nullable): whose chat
 * @cleared: whether /clear has hidden the transcript on this view
 *
 * Returns: (transfer full): the chat view
 */
HtmxElement *clawt_web_chat_body_full(ClawtWebApp *app,
                                      const gchar *agent_id,
                                      gboolean     cleared,
                                      const gchar *peer);
/**
 * clawt_web_room_body:
 * @app: a #ClawtWebApp
 * @room_id: (nullable): which room
 *
 * A group room's chat: the transcript and a composer that posts into
 * the room.  A room has exactly one view, because every other page is
 * scoped to an agent.
 *
 * Returns: (transfer full): the room view
 */
HtmxElement *clawt_web_room_body(ClawtWebApp *app, const gchar *room_id);

HtmxElement *clawt_web_agent_body(ClawtWebApp *app, const gchar *agent_id);
HtmxElement *clawt_web_mailbox_body(ClawtWebApp *app, const gchar *agent_id);
HtmxElement *clawt_web_computer_body(ClawtWebApp *app, const gchar *agent_id);

/**
 * clawt_web_computer_view_body:
 * @app: a #ClawtWebApp
 * @agent_id: (nullable): the selected agent
 * @tab: which of the four sub-views to draw
 *
 * The Computer page showing one of its four halves.
 *
 * Returns: (transfer full): the view
 */
HtmxElement *clawt_web_computer_view_body(ClawtWebApp       *app,
                                          const gchar       *agent_id,
                                          ClawtComputerView  tab);
HtmxElement *clawt_web_routines_body(ClawtWebApp *app, const gchar *agent_id);

/**
 * clawt_web_triggers_body:
 * @app: a #ClawtWebApp
 * @agent_id: (nullable): the selected agent, which triggers ignore
 *
 * Returns: (transfer full): the triggers page
 */
HtmxElement *clawt_web_triggers_body(ClawtWebApp *app, const gchar *agent_id);

/**
 * clawt_web_skills_body:
 * @app: a #ClawtWebApp
 * @agent_id: (nullable): the selected agent, unused here
 *
 * The fleet's skills, each with its provenance and whatever the scan
 * noticed about it.
 *
 * Returns: (transfer full): the view
 */
HtmxElement *clawt_web_skills_body(ClawtWebApp *app, const gchar *agent_id);
HtmxElement *clawt_web_tasks_body(ClawtWebApp *app, const gchar *agent_id);
HtmxElement *clawt_web_flow_body(ClawtWebApp *app, const gchar *agent_id);
HtmxElement *clawt_web_decisions_body(ClawtWebApp *app,
                                      const gchar *agent_id);
HtmxElement *clawt_web_memory_body(ClawtWebApp *app, const gchar *agent_id);

/**
 * clawt_web_section_subnav:
 * @agent_id: (nullable): the selected agent
 * @view: the page being shown
 *
 * The row of page tabs under the topbar for @view's section.
 *
 * Returns: (transfer full) (nullable): the row, or %NULL when the
 *   section holds a single page and there is nothing to choose between
 */
HtmxElement *clawt_web_section_subnav(const gchar *agent_id, ClawtPage view);

/* ── Routes ──────────────────────────────────────────────────────── */

void clawt_web_register_fleet(HtmxRouter *router, ClawtWebApp *app);

/**
 * clawt_web_register_views:
 * @router: the router
 * @app: a #ClawtWebApp
 *
 * The `/a/:id/:view` route, which matches everything under an agent.
 *
 * Must be registered *after* every other route beginning `/a/:id/`, or
 * it takes them: the router answers with the first pattern that matches,
 * and an unrecognised view falls back to chat -- so a swallowed route
 * renders the chat page and returns 200 rather than failing.
 */
void clawt_web_register_views(HtmxRouter *router, ClawtWebApp *app);
void clawt_web_register_chat(HtmxRouter *router, ClawtWebApp *app);
void clawt_web_register_agent(HtmxRouter *router, ClawtWebApp *app);
void clawt_web_register_mailbox(HtmxRouter *router, ClawtWebApp *app);
void clawt_web_register_computer(HtmxRouter *router, ClawtWebApp *app);
void clawt_web_register_work(HtmxRouter *router, ClawtWebApp *app);

/**
 * clawt_web_register_triggers:
 * @router: the router
 * @app: a #ClawtWebApp
 *
 * Registers the trigger routes.
 *
 * They live under `/triggers`, never under `/a/:id`, and are registered
 * before clawt_web_register_views() -- "/a/:id/:view" matches everything
 * below an agent and renders the chat page with a 200, so a route after
 * it is unreachable and looks like a page that renders the wrong thing.
 */
void clawt_web_register_triggers(HtmxRouter *router, ClawtWebApp *app);

/**
 * clawt_web_register_skills:
 * @router: the router
 * @app: a #ClawtWebApp
 *
 * The skills page's actions, and the composer's `/` completions.
 */
void clawt_web_register_skills(HtmxRouter *router, ClawtWebApp *app);
void clawt_web_register_settings(HtmxRouter *router, ClawtWebApp *app);
void clawt_web_register_events(HtmxRouter *router, ClawtWebApp *app);
void clawt_web_register_alerts(HtmxRouter *router, ClawtWebApp *app);

/**
 * clawt_web_register_decisions:
 * @router: the router
 * @app: the client
 *
 * Choices agents need a person to make.  Beside the alerts rather than
 * inside them: an alert is something that happened and a decision is
 * something that needs you, so one badge meaning both would be a badge
 * nobody can act on.
 */
void clawt_web_register_decisions(HtmxRouter *router, ClawtWebApp *app);

/**
 * clawt_web_register_memory:
 * @router: the router
 * @app: a #ClawtWebApp
 *
 * `/memory`: fleet-wide recall and the operator profile.
 *
 * Not under `/a/:id`, because neither is about one agent -- recall
 * searches every room and the profile is the same for the whole fleet.
 * Registered before `/a/:id/:view`, which matches everything below an
 * agent and would swallow it.
 */
void clawt_web_register_memory(HtmxRouter *router, ClawtWebApp *app);
void clawt_web_register_creation(HtmxRouter *router, ClawtWebApp *app);
void clawt_web_register_extras(HtmxRouter *router, ClawtWebApp *app);

/**
 * clawt_web_add_files_card:
 * @app: a #ClawtWebApp
 * @parent: (transfer none): where it goes
 * @agent_id: whose workspace
 *
 * The agent's own org files, and which of them clawtilla writes into.
 */
void clawt_web_add_files_card(ClawtWebApp *app,
                              HtmxElement *parent,
                              const gchar *agent_id);

/**
 * clawt_web_add_memory_card:
 * @app: a #ClawtWebApp
 * @parent: (transfer none): where it goes
 * @agent_id: whose store
 * @query: (nullable): what to search for, or %NULL to list
 *
 * The searchable per-agent memory store.
 */
void clawt_web_add_memory_card(ClawtWebApp *app,
                               HtmxElement *parent,
                               const gchar *agent_id,
                               const gchar *query);

/* ── Shared bits the modules need from each other ────────────────── */

/**
 * clawt_web_send_message:
 * @app: a #ClawtWebApp
 * @request: the request being answered
 * @agent_id: who to send it to
 * @body: what to say
 *
 * Sends one message and re-renders the chat.
 *
 * Shared with /retry, which is the same act with a body read back out of
 * the transcript -- two senders would be two chances for one of them to
 * forget to report the daemon's refusal.
 *
 * Returns: (transfer full): the response
 */
HtmxResponse *clawt_web_send_message(ClawtWebApp *app,
                                     HtmxRequest *request,
                                     const gchar *agent_id,
                                     const gchar *body);

/**
 * clawt_web_param:
 * @params: (element-type utf8 utf8): the route's path parameters
 * @name: which one
 *
 * A path parameter, percent-decoded.
 *
 * The router matches on the raw path, so a parameter arrives still
 * encoded -- and an agent id comes from a config file somebody edits, so
 * it is not necessarily a word.  Reading it raw means an id with a space
 * in it never matches the agent it names.
 *
 * Returns: (transfer full) (nullable): the value
 */
gchar *clawt_web_param(GHashTable *params, const gchar *name);

/**
 * clawt_web_form_value:
 * @request: the request
 * @name: the field
 *
 * A posted field, or %NULL when it was not sent.
 *
 * Returns: (nullable): the value, borrowed from @request
 */
const gchar *clawt_web_form_value(HtmxRequest *request, const gchar *name);

/**
 * clawt_web_form_flag:
 * @request: the request
 * @name: the field
 *
 * Whether a checkbox was ticked.
 *
 * An unticked checkbox posts nothing, which is indistinguishable from a
 * field that was not on the form -- so clawt_web_switch_field() emits a
 * companion "<name>__present" alongside it, and this reads the pair.
 *
 * Returns: %TRUE if the box was ticked
 */
gboolean clawt_web_form_flag(HtmxRequest *request, const gchar *name);

/**
 * clawt_web_form_had:
 * @request: the request
 * @name: the field
 *
 * Whether the form carried @name at all, ticked or not.
 *
 * Returns: %TRUE if the field was present on the page that posted
 */
gboolean clawt_web_form_had(HtmxRequest *request, const gchar *name);

/**
 * clawt_web_after_action:
 * @app: a #ClawtWebApp
 * @request: the request being answered
 * @agent_id: (nullable): where to go back to
 * @view: which view
 * @toast: (nullable): what to say happened
 *
 * Re-renders the whole page after something changed, with a note.
 *
 * A whole page rather than a fragment because an action changes the
 * sidebar, the topbar and the view at once -- three places, which is
 * three chances for one to be left showing the previous answer.
 *
 * Returns: (transfer full): the response
 */
HtmxResponse *clawt_web_after_action(ClawtWebApp  *app,
                                     HtmxRequest  *request,
                                     const gchar  *agent_id,
                                     ClawtPage  view,
                                     const gchar  *toast);

/**
 * clawt_web_room_notice:
 * @app: a #ClawtWebApp
 * @request: the request being answered
 * @room_id: the room to draw underneath
 * @text: (nullable): what to say
 * @tone: (nullable): "bad" for a refusal, %NULL for a plain note
 *
 * The same, for a room.
 *
 * A room has no agent, so clawt_web_after_action() cannot draw it: that
 * one renders the agent view, and passing %NULL leaves somebody looking
 * at "no agent selected" instead of the conversation they were in.
 *
 * Returns: (transfer full): the response
 */
/**
 * clawt_web_run_skill_command:
 * @app: a #ClawtWebApp
 * @request: the request being answered
 * @agent_id: whose skill it is
 * @name: the command, without its leading slash
 * @arguments: (nullable): the rest of the line
 *
 * Expands one of an agent's skill commands and sends the result.
 *
 * The expansion is the daemon's, never the client's, so both clients
 * send identical text for the same `/name args`.
 *
 * Returns: (transfer full): the response
 */
HtmxResponse *clawt_web_run_skill_command(ClawtWebApp  *app,
                                          HtmxRequest  *request,
                                          const gchar  *agent_id,
                                          const gchar  *name,
                                          const gchar  *arguments);

HtmxResponse *clawt_web_room_notice(ClawtWebApp  *app,
                                    HtmxRequest  *request,
                                    const gchar  *room_id,
                                    const gchar  *text,
                                    const gchar  *tone);

/**
 * clawt_web_error_page:
 * @app: a #ClawtWebApp
 * @request: the request being answered
 * @agent_id: (nullable): where the person was
 * @view: which view
 * @message: what went wrong
 *
 * The same page, with the daemon's own refusal shown on it.
 *
 * The daemon's refusals name what to do next -- which setting, which
 * command, which of three places it looked -- so they are shown as
 * written rather than replaced with a generic failure.
 *
 * Returns: (transfer full): the response
 */
HtmxResponse *clawt_web_error_page(ClawtWebApp  *app,
                                   HtmxRequest  *request,
                                   const gchar  *agent_id,
                                   ClawtPage  view,
                                   const gchar  *message);

/**
 * clawt_web_notice:
 * @text: what to say
 * @tone: "" for a warning, "bad" or "info"
 *
 * Returns: (transfer full): a banner
 */
HtmxDiv *clawt_web_notice(const gchar *text, const gchar *tone);

/**
 * clawt_web_first_agent:
 * @app: a #ClawtWebApp
 *
 * Returns: (transfer full) (nullable): the id of the first agent listed
 */
gchar *clawt_web_first_agent(ClawtWebApp *app);

/**
 * clawt_web_find_agent:
 * @app: a #ClawtWebApp
 * @agent_id: which one
 *
 * The agent's entry from `agent.list`.
 *
 * Returns: (transfer full) (nullable): a reply whose "agent" member is
 *   the object, or %NULL when there is no such agent
 */
JsonNode *clawt_web_find_agent(ClawtWebApp *app, const gchar *agent_id);

/**
 * clawt_web_chat_interrupt:
 * @app: the web app
 * @request: the request being answered
 * @agent_id: which agent
 *
 * Kills the CLI carrying out this agent's turn, and everything it
 * spawned, leaving the agent itself up.
 *
 * Shared by the composer's Stop button and the `/interrupt` command, so
 * the two cannot come to say different things about what happened.
 *
 * Returns: (transfer full): the chat page, with what happened on it
 */
HtmxResponse *clawt_web_chat_interrupt(ClawtWebApp *app,
                                       HtmxRequest *request,
                                       const gchar *agent_id);

/**
 * clawt_web_warnings:
 * @parent: (transfer none): where to put them
 * @reply: (nullable): a reply that may carry a "warnings" array
 *
 * Shows whatever the daemon warned about.
 *
 * Fleet-level mistakes -- two leads on one team, an agent naming a team
 * nobody declared -- are warnings rather than errors, and the symptom
 * otherwise is work quietly going nowhere.  So they are shown wherever
 * the daemon reports them.
 *
 * Returns: how many were shown
 */
guint clawt_web_warnings(HtmxElement *parent, JsonNode *reply);

/**
 * clawt_web_sidebar_entries:
 * @app: a #ClawtWebApp
 * @find: (nullable): an id to locate
 * @index_out: (out): where @find sits, or -1
 *
 * The sidebar as one ordered list of typed entries -- `a:<agent>` and
 * `r:<room>` -- which is what a reorder frame carries.
 *
 * Returns: (transfer full) (element-type utf8): the entries
 */
GPtrArray *clawt_web_sidebar_entries(ClawtWebApp *app,
                                     const gchar *find,
                                     gint        *index_out);

/**
 * clawt_web_move_entry:
 * @app: a #ClawtWebApp
 * @entries: (element-type utf8): the list from clawt_web_sidebar_entries()
 * @index: which entry to move
 * @direction: "up" or anything else for down
 * @message: (out) (transfer none): what to say afterwards
 *
 * Returns: %FALSE only when the daemon refused the frame
 */
gboolean clawt_web_move_entry(ClawtWebApp  *app,
                              GPtrArray    *entries,
                              gint          index,
                              const gchar  *direction,
                              const gchar **message);

G_END_DECLS
