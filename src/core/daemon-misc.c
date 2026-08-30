/*
 * daemon-misc.c - The client surface: the verbs with no family of their own
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"

#include <glib/gstdio.h>
#include <string.h>

#include "core/clawt-daemon.h"
#include "core/clawt-daemon-private.h"

/*
 * Says that a decision is no longer waiting.
 *
 * The counterpart of `decision.asked`, and it was missing.  Answering
 * published nothing, so every client's badge went stale the moment
 * *anything else* settled one: a second window, the CLI, or the venture
 * bridge answering on the operator's behalf.  Nothing warned, because a
 * count that is merely too high looks exactly like an inbox somebody
 * has not got to yet.
 *
 * The agent is the subject, matching `decision.asked`, and @how says
 * which of the two ways it ended -- a client that only wants to
 * decrement a counter can ignore it, and one drawing a line in a list
 * cannot.
 */
static void
publish_decision_settled(ClawtDaemon *self, ClawtDecision *decision,
                         const gchar *how)
{
    g_autoptr(ClawtEvent) event = NULL;

    if (self->bus == NULL || decision == NULL)
        return;

    event = clawt_event_new("decision.settled",
                            clawt_decision_get_agent(decision));
    clawt_event_set_detail(event, "decision", clawt_decision_get_id(decision));
    clawt_event_set_detail(event, "how", how);

    clawt_event_bus_publish(self->bus, event);
}

JsonNode *
clawt_daemon_handle_misc(
    ClawtDaemon  *self,
    const gchar  *kind,
    JsonNode     *request,
    JsonObject   *payload,
    gboolean     *handled
)
{
    g_autoptr(JsonBuilder) builder = NULL;
    g_autoptr(GError) error = NULL;

    builder = json_builder_new();
    *handled = TRUE;

    if (g_strcmp0(kind, "attachment.get") == 0) {
        const gchar *id = clawt_ipc_payload_string(payload, "id");
        g_autofree gchar *path = NULL;
        g_autofree gchar *name = NULL;
        g_autofree gchar *contents = NULL;
        g_autofree gchar *encoded = NULL;
        gsize length = 0;

        /*
         * The bytes, not the path.
         *
         * A client may be on another machine entirely -- that is what
         * connection profiles are for -- so handing it a filename would
         * work on this host and show nothing anywhere else, which reads
         * as a broken image rather than as an unsupported setup.
         *
         * The id is checked rather than trusted: clawt_attachment_path()
         * refuses anything outside the character set an id is made of,
         * which is what stops a request for a path of somebody's
         * choosing reading a file this was never meant to serve.
         */
        if (id == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "which attachment?");

        if (self->attachment_dir == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "this daemon keeps no attachments");

        path = clawt_attachment_path(self->attachment_dir, id);

        if (path == NULL || !g_file_get_contents(path, &contents, &length,
                                                 NULL))
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such attachment");

        name = clawt_attachment_name(id);
        encoded = g_base64_encode((const guchar *)contents, length);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, name);
        json_builder_set_member_name(builder, "bytes");
        json_builder_add_int_value(builder, (gint64)length);
        json_builder_set_member_name(builder, "base64");
        json_builder_add_string_value(builder, encoded);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "decision.list") == 0) {
        gboolean open_only = clawt_ipc_payload_boolean(payload, "open", TRUE);
        g_autoptr(GPtrArray) decisions = NULL;
        gint64 now = g_get_real_time() / G_USEC_PER_SEC;
        guint i;

        if (self->decisions == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "this daemon keeps no decisions");

        decisions = clawt_decision_store_list(self->decisions, open_only);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "decisions");
        json_builder_begin_array(builder);

        for (i = 0; decisions != NULL && i < decisions->len; i++)
            clawt_daemon_add_decision_object(
                builder, g_ptr_array_index(decisions, i), now);

        json_builder_end_array(builder);
        json_builder_set_member_name(builder, "open");
        json_builder_add_int_value(
            builder, clawt_decision_store_count_open(self->decisions));
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request,
                                      json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "decision.answer") == 0) {
        const gchar *id = clawt_ipc_payload_string(payload, "decision");
        const gchar *answer = clawt_ipc_payload_string(payload, "answer");
        g_autoptr(ClawtDecision) settled = NULL;
        g_autoptr(GError) answer_error = NULL;

        if (self->decisions == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "this daemon keeps no decisions");

        if (id == NULL || answer == NULL || *answer == '\0')
            return clawt_ipc_error_new(
                request, CLAWT_ERROR_INVALID_ARGUMENT,
                "answering needs a decision and an answer");

        settled = clawt_decision_store_answer(self->decisions, id, answer,
                                              &answer_error);

        if (settled == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                       answer_error->message);

        /*
         * And it goes back to whoever asked.
         *
         * Without this the inbox is a suggestion box: the operator
         * answers into the void and the agent never learns.  Routed as
         * an ordinary message so it costs the agent a turn and reaches
         * it through the machinery everything else uses -- and carrying
         * the task id, so an answer that arrives after the agent has
         * moved on can still be attached to what it was about.
         */
        clawt_daemon_deliver_decision_answer(self, settled);

        /*
         * And back to whichever system staged the change, if one did.
         *
         * A decision raised from VENTURE's queue is only half answered
         * by telling the agent: the change is still sitting there
         * waiting for an approve or a reject, and venture drops it
         * unanswered when its own TTL runs out.  The bridge takes the
         * ones that are its own and says so; every other decision
         * answers here exactly as it always did.
         */
        clawt_daemon_venture_answer(self, settled);

        publish_decision_settled(self, settled, "answered");

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "decision");
        clawt_daemon_add_decision_object(builder, settled,
                                         g_get_real_time() / G_USEC_PER_SEC);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request,
                                      json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "decision.dismiss") == 0) {
        const gchar *id = clawt_ipc_payload_string(payload, "decision");
        g_autoptr(ClawtDecision) dismissed = NULL;
        g_autoptr(GError) dismiss_error = NULL;

        if (self->decisions == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "this daemon keeps no decisions");

        /*
         * Read before it is dismissed, because the turn budget it was
         * holding has to be released and only the record says whose turn
         * that was.  A dismissal that did not release would leave the
         * agent's clock parked for ever on a question nobody is going to
         * answer, which is a turn that can never time out.
         */
        dismissed = clawt_decision_store_get(self->decisions, id);

        if (!clawt_decision_store_dismiss(self->decisions, id,
                                          &dismiss_error))
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       dismiss_error->message);

        if (dismissed != NULL) {
            clawt_daemon_turn_release(self,
                                      clawt_decision_get_agent(dismissed));
            publish_decision_settled(self, dismissed, "dismissed");
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "dismissed");
        json_builder_add_string_value(builder, id);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request,
                                      json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "event.list") == 0) {
        const gchar *subject = clawt_ipc_payload_string(payload, "subject");
        guint limit = (guint)clawt_ipc_payload_int(payload, "limit", 200);
        g_autoptr(GPtrArray) events = NULL;
        guint i;

        /*
         * What the fleet has been doing, from the log that has been
         * recording it all along.
         *
         * ClawtEventLog has written every published event to NDJSON
         * since the daemon was written, sweeps on `daemon.event_log_days`
         * -- and was read back by nobody.  A client can hold the recent
         * ones in memory; anything older than that was on disk and
         * unreachable, which is why diagnosing a message loop meant
         * running sqlite3 and grep on the host.
         *
         * Fleet-wide unless a subject is named, because the case that
         * sends somebody to the shell is watching several agents at
         * once.
         */
        if (self->log == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "this daemon keeps no event log");

        events = clawt_event_log_read(self->log, subject, limit);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "events");
        json_builder_begin_array(builder);

        for (i = 0; events != NULL && i < events->len; i++) {
            ClawtEvent *event = g_ptr_array_index(events, i);
            g_autoptr(JsonNode) node = clawt_ipc_event_new(event);
            JsonObject *frame = json_node_get_object(node);

            /*
             * The event frame's own payload, rather than a second
             * spelling of what an event is.  clawt_ipc_event_new() is
             * what a subscriber receives, so a client reading history
             * and a client receiving live events parse one shape.
             */
            json_builder_add_value(
                builder,
                json_node_ref(json_object_get_member(frame, "payload")));
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "attachment.remove") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        const gchar *name = clawt_ipc_payload_string(payload, "name");
        g_autofree gchar *safe = NULL;
        g_autofree gchar *relative = NULL;
        g_autofree gchar *host_path = NULL;

        if (agent_id == NULL || name == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "agent and name are both required");

        if (self->exchange == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
                                       "there is no exchange directory");

        /*
         * Rebuilt from its basename and resolved through the exchange,
         * exactly as attachment.put does. A client asking to delete
         * "../../../etc/passwd" gets a refusal about a file in its own
         * drop-box that does not exist.
         */
        safe = g_path_get_basename(name);
        relative = g_build_filename(agent_id, safe, NULL);
        host_path = clawt_exchange_resolve(self->exchange, agent_id, relative,
                                           TRUE, &error);

        if (host_path == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        if (g_unlink(host_path) != 0)
            return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                       g_strerror(errno));

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "removed");
        json_builder_add_string_value(builder, host_path);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    /* ── usage ── */
    if (g_strcmp0(kind, "usage.summary") == 0) {
        GPtrArray *agents = clawt_agent_manager_list(self->agents);
        ClawtUsageTotals fleet = { 0, 0, 0, 0 };
        gint64 since = clawt_ipc_payload_int(payload, "since", 0);
        guint i;

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "agents");
        json_builder_begin_array(builder);

        for (i = 0; i < agents->len; i++) {
            ClawtAgent *agent = g_ptr_array_index(agents, i);
            const gchar *agent_id = clawt_agent_get_id(agent);
            g_autofree gchar *state_dir = NULL;
            g_autofree gchar *db_path = NULL;
            g_autoptr(GError) read_error = NULL;
            ClawtUsageTotals totals = { 0, 0, 0, 0 };

            state_dir = clawt_config_agent_state_dir(self->config, agent_id);
            if (state_dir == NULL)
                continue;

            db_path = clawt_usage_database_path(state_dir);

            /*
             * One unreadable database does not fail the summary.  A
             * fleet report that refuses because one agent's file is
             * mid-write tells you nothing about the other nine.
             */
            if (!clawt_usage_read_totals(db_path, since, &totals,
                                         &read_error)) {
                g_debug("usage: %s: %s", agent_id,
                        read_error != NULL ? read_error->message : "unknown");
            }

            clawt_usage_totals_add(&fleet, &totals);

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "id");
            json_builder_add_string_value(builder, agent_id);
            json_builder_set_member_name(builder, "name");
            json_builder_add_string_value(builder,
                                          clawt_agent_get_name(agent));
            json_builder_set_member_name(builder, "turns");
            json_builder_add_int_value(builder, totals.turns);
            json_builder_set_member_name(builder, "input_tokens");
            json_builder_add_int_value(builder, totals.input_tokens);
            json_builder_set_member_name(builder, "output_tokens");
            json_builder_add_int_value(builder, totals.output_tokens);
            json_builder_set_member_name(builder, "cost_micros");
            json_builder_add_int_value(builder, totals.cost_micros);
            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);

        json_builder_set_member_name(builder, "total");
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "turns");
        json_builder_add_int_value(builder, fleet.turns);
        json_builder_set_member_name(builder, "input_tokens");
        json_builder_add_int_value(builder, fleet.input_tokens);
        json_builder_set_member_name(builder, "output_tokens");
        json_builder_add_int_value(builder, fleet.output_tokens);
        json_builder_set_member_name(builder, "cost_micros");
        json_builder_add_int_value(builder, fleet.cost_micros);
        json_builder_end_object(builder);

        json_builder_set_member_name(builder, "since");
        json_builder_add_int_value(builder, since);

        /*
         * What the budget would refuse right now, so a client can show
         * the cap beside the spend rather than making somebody go and
         * read the config to find out what the number means.
         */
        json_builder_set_member_name(builder, "task_budget_usd");
        json_builder_add_double_value(
            builder,
            clawt_config_get_double(self->config,
                                    "orchestration.task_budget_usd"));

        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "attachment.put") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        const gchar *name = clawt_ipc_payload_string(payload, "name");
        const gchar *encoded = clawt_ipc_payload_string(payload, "data");
        ClawtAgent *agent = (agent_id != NULL)
                            ? clawt_agent_manager_get(self->agents, agent_id)
                            : NULL;
        g_autofree guchar *bytes = NULL;
        g_autofree gchar *safe = NULL;
        g_autofree gchar *relative = NULL;
        g_autofree gchar *host_path = NULL;
        gsize length = 0;

        if (agent == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        if (name == NULL || encoded == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "name and data are both required");

        if (self->exchange == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
                                       "there is no exchange directory");

        /*
         * The name is taken apart and rebuilt rather than trusted.  It
         * comes from a filename a person dragged in or a clipboard
         * suggestion, and "../../.ssh/authorized_keys" is a name.
         */
        safe = g_path_get_basename(name);

        if (safe[0] == '\0' || g_strcmp0(safe, ".") == 0 ||
            g_strcmp0(safe, "..") == 0 || g_strcmp0(safe, G_DIR_SEPARATOR_S) == 0)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "that is not a usable file name");

        bytes = g_base64_decode(encoded, &length);

        if (bytes == NULL || length == 0)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "the attachment is empty");

        /*
         * The agent's own directory, made if this is the first thing
         * ever put in it. resolve() answers where a path *would* be, so
         * without this the very first attachment failed on a directory
         * that had never been created.
         */
        if (!clawt_exchange_prepare(self->exchange, agent_id, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        relative = g_build_filename(agent_id, safe, NULL);
        host_path = clawt_exchange_resolve(self->exchange, agent_id, relative,
                                           TRUE, &error);

        if (host_path == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        if (!clawt_write_file_atomic(host_path, (const gchar *)bytes,
                                     (gssize)length, 0600, FALSE, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, safe);
        json_builder_set_member_name(builder, "host_path");
        json_builder_add_string_value(builder, host_path);

        /*
         * The path to *tell the agent*, which is not the host path when
         * it lives in a container: the exchange is mounted, so the
         * agent sees it somewhere else entirely and a host path would
         * send it looking for a file that is not there.
         */
        json_builder_set_member_name(builder, "path");

        {
            const gchar *computer = clawt_agent_config_get_string(
                clawt_agent_get_config(agent), "computer.type");

            if (g_strcmp0(computer, "container") == 0 ||
                g_strcmp0(computer, "vm") == 0) {
                g_autofree gchar *guest = g_build_filename(
                    CLAWT_EXCHANGE_MOUNT_POINT, agent_id, safe, NULL);

                json_builder_add_string_value(builder, guest);
            } else {
                json_builder_add_string_value(builder, host_path);
            }
        }

        json_builder_set_member_name(builder, "bytes");
        json_builder_add_int_value(builder, (gint64)length);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "exchange.list") == 0) {
        g_autoptr(GPtrArray) entries = NULL;
        guint i;

        if (self->exchange == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
                                       "this fleet has no exchange "
                                       "directory");

        entries = clawt_exchange_list(self->exchange,
                                      clawt_ipc_payload_string(payload,
                                                               "path"));

        if (entries == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such directory in the exchange");

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "entries");
        json_builder_begin_array(builder);

        for (i = 0; i < entries->len; i++)
            json_builder_add_string_value(builder,
                                          g_ptr_array_index(entries, i));

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "tool.rpc") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        const gchar *token = clawt_ipc_payload_string(payload, "token");
        JsonNode *rpc = (payload != NULL &&
                         json_object_has_member(payload, "request"))
                        ? json_object_get_member(payload, "request") : NULL;
        g_autoptr(JsonNode) rpc_response = NULL;

        /*
         * The orchestration tools, reachable over IPC.
         *
         * They were served only over the agent's link, as mcp.request
         * frames -- which assumed something on the agent side would
         * relay them into its AI session. Nothing did, and nothing
         * could: an agent runs a CLI whose only way of being given
         * tools is an --mcp-config pointing at a real MCP server. This
         * is the verb clawtilla-mcp-server speaks so that server can
         * exist.
         */
        if (agent_id == NULL || rpc == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "agent and request are both required");

        /*
         * The agent's own token, checked the same way the link checks
         * it. The socket's permissions are the first line; this stops
         * one agent on this machine calling tools as another.
         */
        if (!clawt_daemon_authenticate_agent(agent_id, token, self))
            return clawt_ipc_error_new(request, CLAWT_ERROR_AUTH,
                                       "that is not this agent's token");

        /*
         * computer.exec waits for a command the agent chose, so it is
         * answered from a worker thread rather than from here.  A
         * handler that waited would hold the daemon's main context for
         * the length of that command -- a build, a test run, something
         * reading from a terminal that is not there -- and while it did,
         * every other agent's link, every client, the event stream and
         * the daemon's own SIGTERM would go unserved.  That is the
         * documented rule about IPC handlers and the network, met on the
         * one path where the wait is not a round trip but an arbitrary
         * command.
         */
        if (clawt_mcp_tools_call_defers(self->mcp_tools, agent_id, rpc)) {
            ClawtIpcPending *pending =
                clawt_ipc_server_defer(self->ipc_server, request);

            if (pending == NULL)
                return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                           "this request cannot be "
                                           "answered later");

            clawt_mcp_tools_call_async(
                self->mcp_tools, agent_id, rpc,
                clawt_daemon_on_tool_rpc_finished, pending);

            return NULL;
        }

        rpc_response = clawt_mcp_tools_call(self->mcp_tools, agent_id, rpc);

        if (rpc_response == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                       "the tool produced no response");

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "response");
        json_builder_add_value(builder, json_node_ref(rpc_response));
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "model.list") == 0) {
        const ClawtProviderInfo *catalog;
        gboolean refresh = clawt_ipc_payload_boolean(payload, "refresh",
                                                      FALSE);
        gsize n_providers = 0;
        gsize i;

        catalog = clawt_model_catalog_get(&n_providers);

        /*
         * A stale cache is refreshed behind this request rather than
         * during it. The caller gets whatever is known now; the next
         * one gets the fresh answer.
         */
        if (refresh &&
            (self->model_cache_at == 0 ||
             g_get_monotonic_time() - self->model_cache_at >
                 (gint64)MODEL_CACHE_TTL_SECONDS * G_USEC_PER_SEC))
            clawt_daemon_warm_model_cache(self);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "providers");
        json_builder_begin_array(builder);

        for (i = 0; i < n_providers; i++) {
            gsize j;

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "id");
            json_builder_add_string_value(builder, catalog[i].id);
            json_builder_set_member_name(builder, "label");
            json_builder_add_string_value(builder, catalog[i].label);

            if (catalog[i].note != NULL) {
                json_builder_set_member_name(builder, "note");
                json_builder_add_string_value(builder, catalog[i].note);
            }

            /*
             * Passed on so a client knows to offer a way to type a name
             * that is not listed.  The catalogue is curated and goes
             * stale; nothing validates against it.
             */
            json_builder_set_member_name(builder, "open_ended");
            json_builder_add_boolean_value(builder, catalog[i].open_ended);

            /*
             * Whether libreclaw can actually run an agent on this
             * provider.  Its provider table is command-line only and
             * rewrites anything else to claude-code with a warning, so a
             * client that offers every provider here lets someone pick
             * OpenAI and quietly get Claude Code with "gpt-4o" in the
             * model field.
             */
            json_builder_set_member_name(builder, "agent");
            json_builder_add_boolean_value(builder, catalog[i].agent);

            /*
             * Whether this provider can be given tools, which decides
             * whether it can design an agent.  A client that offers
             * every provider for designing offers ones that will be
             * refused after the person has filled in the whole form.
             */
            json_builder_set_member_name(builder, "tools");
            json_builder_add_boolean_value(builder, catalog[i].tools);

            json_builder_set_member_name(builder, "models");
            json_builder_begin_array(builder);

            /*
             * The provider's own list, when asked for and reachable.
             *
             * The hardcoded table goes stale -- it offered grok-3 and
             * grok-4 well after 4.5 and 4.6 had shipped -- so a person
             * choosing a model should be shown what the provider
             * actually runs. Falls back to the table rather than
             * failing: no key, or no network, is not a reason to offer
             * nothing.
             */
            if (refresh && catalog[i].tools) {
                /*
                 * From the cache, never by asking now.  Asking here made
                 * the request take as long as the slowest provider, and
                 * both the new-agent dialog and the agent inspector ask
                 * on every build -- so pressing + or clicking an agent
                 * appeared to hang.
                 */
                GStrv live = g_hash_table_lookup(self->model_cache,
                                                  catalog[i].id);

                if (live != NULL && live[0] != NULL) {
                    gsize k;

                    for (k = 0; live[k] != NULL; k++) {
                        json_builder_begin_object(builder);
                        json_builder_set_member_name(builder, "id");
                        json_builder_add_string_value(builder, live[k]);
                        json_builder_set_member_name(builder, "label");
                        json_builder_add_string_value(builder, live[k]);
                        json_builder_end_object(builder);
                    }

                    json_builder_end_array(builder);
                    json_builder_set_member_name(builder, "live");
                    json_builder_add_boolean_value(builder, TRUE);
                    json_builder_end_object(builder);
                    continue;
                }
            }

            for (j = 0; j < catalog[i].n_models; j++) {
                json_builder_begin_object(builder);
                json_builder_set_member_name(builder, "id");
                json_builder_add_string_value(builder,
                                              catalog[i].models[j].id);
                json_builder_set_member_name(builder, "label");
                json_builder_add_string_value(builder,
                                              catalog[i].models[j].label);

                if (catalog[i].models[j].note != NULL) {
                    json_builder_set_member_name(builder, "note");
                    json_builder_add_string_value(
                        builder, catalog[i].models[j].note);
                }

                json_builder_end_object(builder);
            }

            json_builder_end_array(builder);
            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    *handled = FALSE;
    return NULL;
}
