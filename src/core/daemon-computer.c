/*
 * daemon-computer.c - The client surface: computer.*
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

JsonNode *
clawt_daemon_handle_computer(
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

    /* ── computers ── */
    if (g_strcmp0(kind, "computer.exec") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        const gchar *command = clawt_ipc_payload_string(payload, "command");
        ClawtAgent *agent = (agent_id != NULL)
                            ? clawt_agent_manager_get(self->agents, agent_id)
                            : NULL;
        ClawtComputer *computer = (agent != NULL)
                                  ? clawt_agent_get_computer(agent) : NULL;
        g_auto(GStrv) argv = NULL;
        ExecJob *job;

        /*
         * The computer is built when the agent starts, so a stopped
         * agent has none -- and "that agent has no computer" then reads
         * as a configuration mistake rather than a stopped agent, which
         * is a different thing to go and check.
         */
        if (computer == NULL) {
            const gchar *configured =
                (agent != NULL)
                ? clawt_agent_config_get_string(clawt_agent_get_config(agent),
                                                "computer.type")
                : NULL;
            g_autofree gchar *detail = NULL;

            if (configured != NULL && g_strcmp0(configured, "none") != 0)
                detail = g_strdup_printf(
                    "%s has a %s computer configured, but it is only built "
                    "when the agent starts. Start it first.",
                    agent_id, configured);
            else
                detail = g_strdup_printf("%s has no computer", agent_id);

            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       detail);
        }

        /*
         * An argv is taken as it stands.  A caller that already has the
         * arguments separated -- the CLI has them from the shell that
         * split them -- must not have them joined and re-split here:
         * `echo 'x\\ny'` came back as `xny`, because the backslash the
         * user quoted was consumed a second time, and `sh -c 'a; b'`
         * turned into four arguments and ran nothing.
         *
         * The string form stays for callers that genuinely have a
         * command line, which is what a model writes.
         */
        argv = clawt_ipc_payload_strv(payload, "argv");

        if (argv != NULL && argv[0] == NULL)
            g_clear_pointer(&argv, g_strfreev);

        /*
         * Only the string form is checked.  A caller that already has
         * the arguments separated means every one of them literally --
         * `find . -exec rm {} ;` is a legitimate argv -- and refusing it
         * would break the CLI for saying exactly what it meant.  The
         * string form is the one a model writes, and the one that has no
         * shell behind it.
         */
        if (argv == NULL && command != NULL) {
            g_autofree gchar *refusal =
                clawt_command_shell_syntax_refusal(command);

            if (refusal != NULL)
                return clawt_ipc_error_new(
                    request, CLAWT_ERROR_INVALID_ARGUMENT, refusal);
        }

        if (argv == NULL &&
            (command == NULL || !g_shell_parse_argv(command, NULL, &argv,
                                                    &error)))
            return clawt_ipc_error_new(
                request, CLAWT_ERROR_INVALID_ARGUMENT,
                error != NULL ? error->message : "no command given");

        job = g_new0(ExecJob, 1);
        job->daemon = g_object_ref(self);
        job->pending = clawt_ipc_server_defer(self->ipc_server, request);

        if (job->pending == NULL) {
            clawt_daemon_exec_job_free(job);
            return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                       "this request cannot be answered "
                                       "later");
        }

        /*
         * A reference of its own, for the same reason the tool path takes
         * one: the agent can be stopped while the command is still
         * running, and the worker must not be left holding a computer the
         * manager has dropped.
         */
        job->computer = g_object_ref(computer);
        job->agent_id = g_strdup(agent_id);

        /*
         * The command is copied because the audit line is written when it
         * *ends*, and by then the request frame it was read from is gone.
         * An argv caller has no command string, so the trail records what
         * it would have been rather than nothing -- a record that says a
         * command ran and not which one answers the wrong question.
         */
        job->command = (command != NULL)
                       ? g_strdup(command)
                       : g_strjoinv(" ", argv);

        clawt_computer_exec_async(
            computer, (const gchar * const *)argv,
            clawt_ipc_payload_string(payload, "cwd"),
            (guint)clawt_ipc_payload_int(payload, "timeout", 120),
            self->main_context, NULL, clawt_daemon_on_ipc_exec_finished, job);

        /*
         * NULL, not a frame.  The answer goes out from
         * clawt_daemon_on_ipc_exec_finished() when the command ends;
         * waiting here would hold the daemon's main context for the
         * whole timeout, which is the two minutes in which nothing else
         * is routed.
         */
        return NULL;
    }

    if (g_strcmp0(kind, "computer.start") == 0 ||
        g_strcmp0(kind, "computer.stop") == 0 ||
        g_strcmp0(kind, "computer.restart") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        ClawtAgent *agent = (agent_id != NULL)
                            ? clawt_agent_manager_get(self->agents, agent_id)
                            : NULL;
        ClawtAgentConfig *agent_config = (agent_id != NULL)
            ? clawt_config_get_agent(self->config, agent_id) : NULL;
        g_autoptr(ClawtComputer) built = NULL;
        ClawtComputer *computer;
        ClawtComputerLifecycle op;
        ClawtComputerType type;
        LifecycleJob *job;

        if (agent_config == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        op = (g_strcmp0(kind, "computer.start") == 0)
             ? CLAWT_COMPUTER_LIFECYCLE_START
             : (g_strcmp0(kind, "computer.stop") == 0)
               ? CLAWT_COMPUTER_LIFECYCLE_STOP
               : CLAWT_COMPUTER_LIFECYCLE_RESTART;

        type = (ClawtComputerType)clawt_agent_config_get_enum(
            agent_config, "computer.type");

        /*
         * Refused here rather than by the backend, and on the *type*
         * rather than on whether the vfunc happens to exist.  A host
         * agent's machine is the one clawtilla is running on: there is a
         * host_stop(), it is a no-op, and answering "stopped" about the
         * operator's own workstation is exactly the quiet lie this path
         * exists to avoid. Both clients ask the same predicate before
         * offering the verb, so a type added later reaches all three
         * without any of them being edited.
         */
        if (!clawt_computer_type_has_machine(type))
            return clawt_ipc_error_new(
                request, CLAWT_ERROR_NOT_SUPPORTED,
                (type == CLAWT_COMPUTER_HOST)
                ? "a host agent has no machine of its own to start or "
                  "stop: it runs on this one"
                : "this agent has no computer");

        /*
         * A fence, because for a container this is not reversible.
         * `computer.container.keep` is false by default and the backend
         * removes the container when it stops one -- so the contents are
         * gone, not merely offline, and "stop" is not a word anybody
         * reads that way. Refused rather than done carefully, and the
         * refusal names both the flag to pass and the setting that would
         * make it unnecessary.
         */
        if (op != CLAWT_COMPUTER_LIFECYCLE_START &&
            clawt_daemon_computer_stop_removes(agent_config) &&
            !clawt_ipc_payload_boolean(payload, "remove", FALSE)) {
            g_autofree gchar *detail = g_strdup_printf(
                "stopping this %s removes it and everything in it, because "
                "computer.%s.keep is false. Pass remove to go ahead, or set "
                "that key to keep what the agent installed.",
                clawt_enum_to_nick(CLAWT_TYPE_COMPUTER_TYPE, type),
                clawt_enum_to_nick(CLAWT_TYPE_COMPUTER_TYPE, type));

            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       detail);
        }

        /*
         * The agent's own computer when it has one, so its state and the
         * machine's stay the same object.  A stopped agent has none --
         * the computer is built at start -- and a machine outliving its
         * agent is the ordinary case here rather than an edge one: a
         * libvirt domain survives the daemon, and a container with
         * keep: true survives everything. So one is built from the
         * config, the same way computer.rebuild does.
         */
        computer = (agent != NULL) ? clawt_agent_get_computer(agent) : NULL;

        if (computer == NULL) {
            g_autoptr(GPtrArray) defaults =
                clawt_config_get_default_mounts(self->config);

            built = clawt_computer_factory_create(agent_config, defaults,
                                                  self->pod_bridge, &error);

            if (built == NULL)
                return clawt_ipc_error_new(request, error->code,
                                           error->message);

            computer = built;
        }

        job = g_new0(LifecycleJob, 1);
        job->daemon = g_object_ref(self);
        job->pending = clawt_ipc_server_defer(self->ipc_server, request);

        if (job->pending == NULL) {
            clawt_daemon_lifecycle_job_free(job);
            return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                       "this request cannot be answered "
                                       "later");
        }

        job->computer = g_object_ref(computer);
        job->agent_id = g_strdup(agent_id);
        job->op = op;
        job->removes = clawt_daemon_computer_stop_removes(agent_config);

        clawt_computer_lifecycle_async(
            computer, op, self->main_context, NULL,
            clawt_daemon_on_ipc_lifecycle_finished, job);

        /*
         * NULL, not a frame.  Starting a container is a blocking request
         * to podman and stopping a VM waits up to thirty seconds for the
         * guest to flush; either held here is thirty seconds in which
         * nothing else is routed.
         */
        return NULL;
    }

    if (g_strcmp0(kind, "computer.rebuild") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        ClawtAgent *agent = (agent_id != NULL)
                            ? clawt_agent_manager_get(self->agents, agent_id)
                            : NULL;
        ClawtAgentConfig *agent_config = (agent_id != NULL)
            ? clawt_config_get_agent(self->config, agent_id) : NULL;
        g_autoptr(ClawtComputer) built = NULL;
        g_autoptr(GError) teardown_error = NULL;
        g_autofree gchar *removed = NULL;

        if (agent_config == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        /*
         * Refused while it runs, rather than done carefully.  Rebuilding
         * is destroying the machine the agent is working on; there is no
         * version of that which is safe to do underneath it.
         */
        if (agent != NULL &&
            clawt_agent_get_state(agent) != CLAWT_AGENT_STATE_STOPPED)
            return clawt_ipc_error_new(request, CLAWT_ERROR_AGENT_STATE,
                                       "stop the agent first: rebuilding "
                                       "destroys the computer it is using");

        if ((ClawtComputerType)clawt_agent_config_get_enum(
                agent_config, "computer.type") == CLAWT_COMPUTER_NONE)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
                                       "this agent has no computer to "
                                       "rebuild");

        /*
         * Built from the config rather than taken from the agent: a
         * stopped agent has no computer object, and stopped is the only
         * state this is allowed in.
         */
        {
            g_autoptr(GPtrArray) defaults =
                clawt_config_get_default_mounts(self->config);

            built = clawt_computer_factory_create(agent_config, defaults,
                                                  self->pod_bridge, &error);
        }

        if (built == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        /*
         * A teardown that fails is reported and not fatal.  The common
         * reason to reach for this is that the guest is already gone --
         * deleted by hand in virt-manager -- and refusing to rebuild
         * because there was nothing to tear down would be absurd.
         */
        if (!clawt_computer_teardown(built, &teardown_error)) {
            removed = g_strdup(teardown_error->message);
            g_message("agent %s: nothing to tear down before rebuilding "
                      "(%s)", agent_id, removed);
        }

        if (!clawt_computer_provision(built, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        clawt_event_bus_emit(self->bus, "agent.changed", agent_id);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "rebuilt");
        json_builder_add_boolean_value(builder, TRUE);
        clawt_daemon_add_string_member(builder, "agent", agent_id);

        if (removed != NULL)
            clawt_daemon_add_string_member(builder, "note", removed);

        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "computer.status") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        ClawtAgent *agent = (agent_id != NULL)
                            ? clawt_agent_manager_get(self->agents, agent_id)
                            : NULL;
        ClawtComputer *computer = (agent != NULL)
                                  ? clawt_agent_get_computer(agent) : NULL;
        g_autofree gchar *described = NULL;

        if (computer == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "that agent has no computer");

        described = clawt_agent_describe_computer(agent);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "state");
        json_builder_add_string_value(
            builder, clawt_enum_to_nick(CLAWT_TYPE_COMPUTER_STATE,
                                        clawt_computer_get_state(computer)));
        json_builder_set_member_name(builder, "description");
        json_builder_add_string_value(builder, described);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "computer.desktop") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        ClawtAgent *agent = (agent_id != NULL)
                            ? clawt_agent_manager_get(self->agents, agent_id)
                            : NULL;
        ClawtComputer *computer = (agent != NULL)
                                  ? clawt_agent_get_computer(agent) : NULL;
        ClawtAgentConfig *agent_config = (agent != NULL)
                                         ? clawt_agent_get_config(agent)
                                         : NULL;
        g_autoptr(ClawtDesktop) built = NULL;
        ClawtDesktop *desktop = NULL;
        g_auto(GStrv) argv = NULL;
        g_auto(GStrv) tools = NULL;
        gsize i;

        if (agent == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "there is no such agent");

        /*
         * The attached desktop when there is one, and otherwise one built
         * from the config.
         *
         * An agent only gets a ClawtDesktop when it is started, so a
         * stopped agent with the grant plainly set was told it "has no
         * desktop; set computer.desktop.enabled" -- naming the key that
         * was already true. The policy is a pure function of the config,
         * so it can be answered without the agent running.
         */
        desktop = clawt_agent_get_desktop(agent);

        if (desktop == NULL) {
            built = clawt_computer_factory_create_desktop(agent_config);
            desktop = built;
        }

        if (desktop == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "that agent has no desktop; set "
                                       "computer.desktop.enabled");

        if (clawt_agent_config_get_enum(agent_config, "computer.type") !=
            CLAWT_COMPUTER_VM)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
                                       "that agent's desktop is not in a "
                                       "VM, so there is nothing to relay "
                                       "to");

        /*
         * Configured for a VM but without one built means the agent is
         * not running, which is a different thing from being misconfigured
         * and deserves saying so.
         */
        if (computer == NULL ||
            clawt_computer_get_computer_type(computer) != CLAWT_COMPUTER_VM)
            return clawt_ipc_error_new(request, CLAWT_ERROR_AGENT_STATE,
                                       "that agent is not running, so its "
                                       "VM has no address yet. Start the "
                                       "agent first.");

        /*
         * Built here rather than written into the agent's .mcp.json,
         * because the port that reaches the guest is chosen when the VM
         * is provisioned -- which is after the workspace files are
         * written, and again after anybody edits the config. A command
         * line captured at render time would name a port nothing is
         * listening on.
         */
        argv = clawt_vm_computer_build_desktop_argv(CLAWT_VM_COMPUTER(computer));

        if (argv == NULL)
            return clawt_ipc_error_new(
                request, CLAWT_ERROR_COMPUTER_EXEC,
                "nothing reaches that agent's VM yet: it may not be "
                "running, or no port is forwarded to it. Start the agent "
                "and try again.");

        tools = clawt_desktop_get_tool_names(desktop);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "backend");
        json_builder_add_string_value(
            builder, clawt_enum_to_nick(CLAWT_TYPE_DESKTOP_BACKEND,
                                        clawt_desktop_resolve_backend(desktop,
                                                                      NULL)));

        json_builder_set_member_name(builder, "argv");
        json_builder_begin_array(builder);
        for (i = 0; argv[i] != NULL; i++)
            json_builder_add_string_value(builder, argv[i]);
        json_builder_end_array(builder);

        /*
         * The permitted tools travel with the command, so the relay does
         * not need its own copy of the policy -- and so an agent whose
         * allow_input was turned off stops being able to click the moment
         * the daemon is reloaded, rather than whenever its MCP client is
         * next restarted.
         */
        json_builder_set_member_name(builder, "tools");
        json_builder_begin_array(builder);
        for (i = 0; tools != NULL && tools[i] != NULL; i++)
            json_builder_add_string_value(builder, tools[i]);
        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "computer.copy") == 0) {
        const gchar *src = clawt_ipc_payload_string(payload, "src");
        const gchar *dst = clawt_ipc_payload_string(payload, "dst");
        g_auto(GStrv) src_parts = NULL;
        g_auto(GStrv) dst_parts = NULL;
        ClawtAgent *agent;
        ClawtComputer *computer;
        gboolean ok;

        if (src == NULL || dst == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "src and dst are both required");

        /*
         * Exactly one side may name an agent.  Copying between two agents
         * would need a temporary file on the host and a policy about who
         * owns it; the exchange directory already solves that case, and
         * saying so is better than half-implementing it.
         */
        src_parts = g_strsplit(src, ":", 2);
        dst_parts = g_strsplit(dst, ":", 2);

        if (g_strv_length(src_parts) == 2 && g_strv_length(dst_parts) == 2)
            return clawt_ipc_error_new(
                request, CLAWT_ERROR_NOT_SUPPORTED,
                "copying straight between two agents is not supported; "
                "copy through the exchange directory instead");

        /*
         * A copy into the exchange goes through the exchange's own rule,
         * which is what says an agent may write to shared/ and its own
         * directory and nowhere else.  Skipping it -- as this used to --
         * let any agent overwrite another's drop-box, the exact thing the
         * rule exists to prevent.
         */
        if (self->exchange != NULL && g_strv_length(dst_parts) == 2 &&
            g_str_has_prefix(dst_parts[1], CLAWT_EXCHANGE_MOUNT_POINT)) {
            g_autofree gchar *resolved =
                clawt_exchange_resolve(self->exchange, dst_parts[0],
                                       dst_parts[1], TRUE, &error);

            if (resolved == NULL)
                return clawt_ipc_error_new(request, error->code,
                                           error->message);
        }

        if (g_strv_length(src_parts) == 2) {
            agent = clawt_agent_manager_get(self->agents, src_parts[0]);
            computer = (agent != NULL) ? clawt_agent_get_computer(agent)
                                       : NULL;

            if (computer == NULL)
                return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                           "that agent has no computer");

            ok = clawt_computer_get_file(computer, src_parts[1], dst, &error);
        } else if (g_strv_length(dst_parts) == 2) {
            agent = clawt_agent_manager_get(self->agents, dst_parts[0]);
            computer = (agent != NULL) ? clawt_agent_get_computer(agent)
                                       : NULL;

            if (computer == NULL)
                return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                           "that agent has no computer");

            ok = clawt_computer_put_file(computer, src, dst_parts[1], &error);
        } else {
            return clawt_ipc_error_new(
                request, CLAWT_ERROR_INVALID_ARGUMENT,
                "one side must be <agent>:<path>");
        }

        if (!ok)
            return clawt_ipc_error_new(request, error->code, error->message);

        return clawt_ipc_response_new(request, NULL);
    }

    *handled = FALSE;
    return NULL;
}
