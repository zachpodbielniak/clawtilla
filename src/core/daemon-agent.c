/*
 * daemon-agent.c - The client surface: agent.* and memory.*
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
clawt_daemon_handle_agent(
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

    /* ── agents ── */
    if (g_strcmp0(kind, "agent.list") == 0) {
        GPtrArray *agents = clawt_agent_manager_list(self->agents);
        guint i;

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "agents");
        json_builder_begin_array(builder);

        {
            /*
             * Sorted here rather than in the manager, which keeps the
             * fleet in the order the file has it -- that order is what a
             * tie falls back to, so it has to survive.
             */
            g_autoptr(GPtrArray) ordered = g_ptr_array_new();

            for (i = 0; i < agents->len; i++)
                g_ptr_array_add(ordered, g_ptr_array_index(agents, i));

            {
                g_autoptr(GPtrArray) teams =
                    clawt_config_get_teams(self->config);

                g_ptr_array_sort_with_data(ordered,
                                           clawt_daemon_compare_by_order,
                                           teams);
            }

            for (i = 0; i < ordered->len; i++)
                clawt_daemon_add_agent_object(builder,
                                              g_ptr_array_index(ordered, i));
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "agent.show") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        ClawtAgent *agent = (agent_id != NULL)
                            ? clawt_agent_manager_get(self->agents, agent_id)
                            : NULL;
        ClawtComputer *computer;

        if (agent == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "agent");
        clawt_daemon_add_agent_object(builder, agent);

        /*
         * Every settable key, so a client can build an editor from the
         * schema instead of from a list of its own. The GTK inspector
         * predates this and names its rows by hand, which is why a
         * setting added to the schema shows up there only when somebody
         * remembers to add a row for it.
         */
        clawt_daemon_add_agent_settings(builder, agent);

        computer = clawt_agent_get_computer(agent);

        if (computer != NULL) {
            g_autofree gchar *described =
                clawt_agent_describe_computer(agent);

            json_builder_set_member_name(builder, "computer_detail");
            json_builder_add_string_value(builder, described);
        }

        /*
         * What the persona costs, and where the cost is.
         *
         * Always reported, so a client never has to decide whether to
         * ask; `verdict` is what decides whether there is anything to
         * *say*, and it is absent below the threshold.  A byte count on
         * every agent is noise, and noise is what stops the one that
         * matters from being read.
         */
        {
            g_autoptr(ClawtIdentitySize) size =
                clawt_workspace_measure_identity(clawt_agent_get_config(agent));
            g_autofree gchar *verdict =
                clawt_workspace_identity_verdict(size);
            guint i;

            json_builder_set_member_name(builder, "identity");
            json_builder_begin_object(builder);

            json_builder_set_member_name(builder, "bytes");
            json_builder_add_int_value(builder, (gint64)size->total);
            json_builder_set_member_name(builder, "limit");
            json_builder_add_int_value(builder, (gint64)size->limit);

            if (verdict != NULL) {
                json_builder_set_member_name(builder, "verdict");
                json_builder_add_string_value(builder, verdict);
            }

            json_builder_set_member_name(builder, "files");
            json_builder_begin_array(builder);

            for (i = 0; i < size->files->len; i++) {
                ClawtIdentityFile *file = g_ptr_array_index(size->files, i);

                json_builder_begin_object(builder);
                json_builder_set_member_name(builder, "name");
                json_builder_add_string_value(builder, file->name);
                json_builder_set_member_name(builder, "bytes");
                json_builder_add_int_value(builder, (gint64)file->bytes);
                json_builder_set_member_name(builder, "present");
                json_builder_add_boolean_value(builder, file->present);
                json_builder_end_object(builder);
            }

            json_builder_end_array(builder);
            json_builder_end_object(builder);
        }

        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "agent.start") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");

        if (!clawt_daemon_start_agent(self, agent_id, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        return clawt_ipc_response_new(request, NULL);
    }

    if (g_strcmp0(kind, "agent.stop") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "stopped");
        json_builder_add_boolean_value(
            builder, clawt_daemon_stop_agent(self, agent_id, TRUE));
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "agent.interrupt") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        guint killed = 0;

        if (!clawt_daemon_interrupt_agent(self, agent_id, &killed, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "interrupted");
        json_builder_add_boolean_value(builder, TRUE);

        /*
         * The count, because zero and several mean different things to
         * whoever pressed the button: nothing was running, or that much
         * was. A bare "done" reads the same either way, and the client
         * has to say which.
         */
        json_builder_set_member_name(builder, "killed");
        json_builder_add_int_value(builder, (gint64)killed);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "agent.restart") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");

        /*
         * The machine goes down with it and comes back up with the
         * start.  Blocking, in the same way clawt_daemon_start_agent()
         * is and for the reason written there: every caller of this one
         * has somebody waiting on the answer, and the wait is bounded by
         * podomation's socket timeout. It is the *fleet* coming up that
         * must not hold the loop.
         */
        clawt_daemon_stop_agent(self, agent_id, TRUE);

        if (!clawt_daemon_start_agent(self, agent_id, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        return clawt_ipc_response_new(request, NULL);
    }

    /*
     * Reading and writing one workspace file.
     *
     * The GTK client opens these in $EDITOR, which is a local program on
     * the machine a person is sitting at -- so a client reached over the
     * network has no way to offer the same thing without a wire path.
     * These are that path, and nothing else uses them.
     *
     * The name goes through clawt_workspace_file_path(), which refuses
     * anything containing a separator or "..": this is reached from an
     * IPC request, and a client that could name "../../secrets" would be
     * reading another agent's credentials.
     */
    if (g_strcmp0(kind, "agent.file_read") == 0 ||
        g_strcmp0(kind, "agent.file_write") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        const gchar *name = clawt_ipc_payload_string(payload, "name");
        ClawtAgentConfig *config = (agent_id != NULL)
            ? clawt_config_get_agent(self->config, agent_id) : NULL;
        g_autofree gchar *path = NULL;

        if (config == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        if (name == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "name is required");

        path = clawt_workspace_file_path(config, name);

        if (path == NULL)
            return clawt_ipc_error_new(
                request, CLAWT_ERROR_INVALID_ARGUMENT,
                "that is not a plain file name inside the workspace");

        if (g_strcmp0(kind, "agent.file_write") == 0) {
            const gchar *content = clawt_ipc_payload_string(payload,
                                                            "content");

            if (content == NULL)
                return clawt_ipc_error_new(request,
                                           CLAWT_ERROR_INVALID_ARGUMENT,
                                           "content is required");

            if (!g_file_set_contents(path, content, -1, &error))
                return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                           error->message);

            clawt_event_bus_emit(self->bus, "agent.changed", agent_id);

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "path");
            json_builder_add_string_value(builder, path);
            json_builder_set_member_name(builder, "bytes");
            json_builder_add_int_value(builder, (gint64)strlen(content));
            json_builder_end_object(builder);

            return clawt_ipc_response_new(request,
                                          json_builder_get_root(builder));
        }

        {
            g_autofree gchar *content = NULL;

            /*
             * A file that is not there yet is empty rather than an
             * error. The standard set is scaffolded at first start, so
             * asking for one before then is an ordinary thing to do --
             * and an editor that refused to open a file it is about to
             * create would be a strange editor.
             */
            if (!g_file_get_contents(path, &content, NULL, NULL))
                content = g_strdup("");

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "name");
            json_builder_add_string_value(builder, name);
            json_builder_set_member_name(builder, "path");
            json_builder_add_string_value(builder, path);
            json_builder_set_member_name(builder, "content");
            json_builder_add_string_value(builder, content);
            json_builder_end_object(builder);

            return clawt_ipc_response_new(request,
                                          json_builder_get_root(builder));
        }
    }

    if (g_strcmp0(kind, "agent.files") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        ClawtAgent *agent = (agent_id != NULL)
                            ? clawt_agent_manager_get(self->agents, agent_id)
                            : NULL;
        ClawtAgentConfig *agent_config;
        const ClawtWorkspaceFile *files;
        guint n_files = 0;
        guint i;

        if (agent == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        agent_config = clawt_agent_get_config(agent);

        /*
         * Scaffolded on the way out, so `agent edit` works on an agent
         * that has never been started.  Nothing is overwritten.
         */
        if (!clawt_workspace_scaffold(agent_config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        /*
         * .mcp.json too, so `agent edit <id> .mcp.json` opens a real
         * file on an agent that has never been started.  It is written
         * here rather than by the full render because that resolves
         * credentials, which can run a command, and a handler runs on
         * the daemon's main context while the client waits.
         */
        {
            g_autofree gchar *state_dir = clawt_config_agent_state_dir(
                self->config, clawt_agent_config_get_id(agent_config));
            g_autofree gchar *socket_path =
                clawt_config_get_path_value(self->config, "daemon.socket");

            if (!clawt_workspace_write_mcp_config(self->config, agent_config,
                                                  socket_path, state_dir,
                                                  &error))
                return clawt_ipc_error_new(request, error->code,
                                           error->message);
        }

        files = clawt_workspace_files(&n_files);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "workspace");

        {
            g_autofree gchar *workspace =
                clawt_agent_config_get_workspace(agent_config);

            json_builder_add_string_value(builder, workspace);
        }

        json_builder_set_member_name(builder, "files");
        json_builder_begin_array(builder);

        for (i = 0; i < n_files; i++) {
            g_autofree gchar *path =
                clawt_workspace_file_path(agent_config, files[i].name);

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "name");
            json_builder_add_string_value(builder, files[i].name);
            json_builder_set_member_name(builder, "path");
            json_builder_add_string_value(builder, path != NULL ? path : "");
            json_builder_set_member_name(builder, "title");
            json_builder_add_string_value(builder, files[i].title);
            json_builder_set_member_name(builder, "identity");
            json_builder_add_boolean_value(builder, files[i].identity);
            json_builder_set_member_name(builder, "generated");
            json_builder_add_boolean_value(builder, files[i].generated);
            json_builder_set_member_name(builder, "exists");
            json_builder_add_boolean_value(
                builder,
                path != NULL && g_file_test(path, G_FILE_TEST_EXISTS));
            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "agent.logs") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        ClawtAgent *agent = (agent_id != NULL)
                            ? clawt_agent_manager_get(self->agents, agent_id)
                            : NULL;
        ClawtAgentRuntime *runtime;
        g_auto(GStrv) lines = NULL;
        gsize i;

        if (agent == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        runtime = clawt_agent_get_runtime(agent);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "lines");
        json_builder_begin_array(builder);

        if (runtime != NULL) {
            lines = clawt_agent_runtime_get_log_tail(
                runtime,
                (guint)clawt_ipc_payload_int(payload, "limit", 200));

            for (i = 0; lines != NULL && lines[i] != NULL; i++)
                json_builder_add_string_value(builder, lines[i]);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "memory.list") == 0 ||
        g_strcmp0(kind, "memory.search") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        ClawtAgent *agent = (agent_id != NULL)
                            ? clawt_agent_manager_get(self->agents, agent_id)
                            : NULL;
        ClawtMemoryStore *store;
        g_autoptr(GPtrArray) memories = NULL;
        guint i;

        if (agent == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        store = clawt_agent_get_memory(agent);

        if (store == NULL)
            return clawt_ipc_error_new(
                request, CLAWT_ERROR_NOT_SUPPORTED,
                "that agent has no memory store; memories.enabled is off");

        if (g_strcmp0(kind, "memory.search") == 0)
            memories = clawt_memory_store_search(
                store, clawt_ipc_payload_string(payload, "query"),
                clawt_ipc_payload_string(payload, "category"),
                (guint)clawt_ipc_payload_int(payload, "limit", 20), NULL);
        else
            memories = clawt_memory_store_list(
                store, clawt_ipc_payload_string(payload, "category"),
                clawt_ipc_payload_boolean(payload, "pinned", FALSE),
                (guint)clawt_ipc_payload_int(payload, "limit", 20), NULL);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "total");
        json_builder_add_int_value(builder,
                                   clawt_memory_store_count(store, FALSE));
        json_builder_set_member_name(builder, "memories");
        json_builder_begin_array(builder);

        for (i = 0; memories != NULL && i < memories->len; i++) {
            ClawtMemory *memory = g_ptr_array_index(memories, i);

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "id");
            json_builder_add_string_value(builder, memory->id);
            json_builder_set_member_name(builder, "content");
            json_builder_add_string_value(builder, memory->content);

            if (memory->summary != NULL) {
                json_builder_set_member_name(builder, "summary");
                json_builder_add_string_value(builder, memory->summary);
            }

            json_builder_set_member_name(builder, "category");
            json_builder_add_string_value(builder, memory->category);
            json_builder_set_member_name(builder, "importance");
            json_builder_add_string_value(builder, memory->importance);

            if (memory->tags != NULL) {
                json_builder_set_member_name(builder, "tags");
                json_builder_add_string_value(builder, memory->tags);
            }

            json_builder_set_member_name(builder, "pinned");
            json_builder_add_boolean_value(builder, memory->pinned);
            json_builder_set_member_name(builder, "created_at");
            json_builder_add_int_value(builder, memory->created_at);
            json_builder_set_member_name(builder, "access_count");
            json_builder_add_int_value(builder, memory->access_count);
            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "agent.reset") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        ClawtAgent *agent = (agent_id != NULL)
                            ? clawt_agent_manager_get(self->agents, agent_id)
                            : NULL;
        g_autofree gchar *state_dir = NULL;
        g_autofree gchar *sessions = NULL;
        g_autofree gchar *aside = NULL;
        g_autofree gchar *db_path = NULL;
        gboolean was_running;
        guint cleared = 0;

        if (agent == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        was_running = clawt_agent_get_state(agent) != CLAWT_AGENT_STATE_STOPPED;

        /*
         * Stopped first, and not only to be tidy: the agent holds its
         * own session files and its own sqlite connection open, and
         * clearing either underneath a running process is how you get a
         * half-reset session that resumes anyway.
         */
        if (was_running)
            clawt_daemon_stop_agent(self, agent_id, FALSE);

        state_dir = clawt_config_agent_state_dir(self->config, agent_id);
        sessions = g_build_filename(state_dir, "sessions", NULL);
        db_path = clawt_usage_database_path(state_dir);

        /*
         * Moved aside rather than deleted. A reset is what you reach for
         * when something is wedged, which is exactly when you might want
         * to look at what it was doing.
         */
        if (g_file_test(sessions, G_FILE_TEST_IS_DIR)) {
            aside = g_strdup_printf("%s.reset-%" G_GINT64_FORMAT, sessions,
                                    g_get_real_time() / G_USEC_PER_SEC);

            if (g_rename(sessions, aside) != 0)
                g_clear_pointer(&aside, g_free);
        }

        /*
         * And the database rows, because libreclaw restores a session
         * from either place -- clearing only the files leaves the agent
         * resuming the same CLI session from sqlite and looking like the
         * reset did nothing.
         *
         * Through libreclaw's own API rather than by opening its schema:
         * the daemon links liblc, and the agent is stopped, so this is
         * the same code the agent itself would run.
         *
         * The path comes from clawt_usage_database_path() because this
         * block spelled it itself for a long time, as
         * `<state_dir>/libreclaw.db` -- which is not where libreclaw
         * puts it.  Its sqlite backend builds the name from
         * `session.persist_dir` and never reads `database.path`, so the
         * file tested for here has never existed on any machine: the
         * branch was skipped every time and `sessions_cleared` was
         * always 0.  Reset appeared to work only because moving the
         * sessions directory aside takes the database with it, which is
         * luck rather than the two-places-to-clear this was written for.
         */
        if (g_file_test(db_path, G_FILE_TEST_EXISTS)) {
            g_autoptr(LcDatabase) db = LC_DATABASE(lc_sqlite_database_new());
            g_autoptr(GError) db_error = NULL;

            if (lc_database_open(db, db_path, &db_error)) {
                GPtrArray *rows = lc_database_load_sessions(db, NULL);
                guint i;

                for (i = 0; rows != NULL && i < rows->len; i++) {
                    LcDbSession *row = g_ptr_array_index(rows, i);

                    if (lc_database_remove_session(db, row->session_key,
                                                   NULL))
                        cleared++;
                }

                g_clear_pointer(&rows, g_ptr_array_unref);
                lc_database_close(db);
            } else {
                g_warning("agent %s: could not clear its session rows: %s",
                          agent_id, db_error->message);
            }
        }

        /*
         * The database that comes back is a new one, numbering its rows
         * from 1 again.  A watermark from the old one would suppress
         * every row in it, so the agent would appear to spend nothing
         * ever again.
         */
        if (self->usage != NULL)
            clawt_usage_forget(self->usage, agent_id);

        if (was_running && !clawt_daemon_start_agent(self, agent_id, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "sessions_cleared");
        json_builder_add_int_value(builder, cleared);
        json_builder_set_member_name(builder, "moved");
        json_builder_add_string_value(builder, aside != NULL ? aside : "");
        json_builder_set_member_name(builder, "restarted");
        json_builder_add_boolean_value(builder, was_running);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "agent.discover") == 0) {
        g_autofree gchar *agents_dir = NULL;
        g_autofree gchar *workspace_root = NULL;
        g_autoptr(GHashTable) seen = NULL;
        gsize d;
        static const gchar *interesting[] = {
            "mailbox.db", "memory.db", "config.yaml", "SOUL.org",
            "IDENTITY.org", "AGENTS.md", NULL
        };

        /*
         * Directories that look like agents but are not in the config.
         *
         * They accumulate: an agent removed from the config keeps its
         * state, a design that was never committed leaves a workspace,
         * and a config restored from a backup leaves everything it did
         * not mention. None of that is visible anywhere, so it just
         * sits on disk and surprises people.
         */
        agents_dir = g_build_filename(self->state_dir, "agents", NULL);
        workspace_root = clawt_config_get_path_value(
            self->config, "defaults.workspace_root");

        seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "found");
        json_builder_begin_array(builder);

        for (d = 0; d < 2; d++) {
            const gchar *root = (d == 0) ? agents_dir : workspace_root;
            g_autoptr(GDir) dir = NULL;
            const gchar *name;

            if (root == NULL)
                continue;

            /* One root may be inside the other; do not list twice. */
            if (d == 1 && g_strcmp0(root, agents_dir) == 0)
                continue;

            dir = g_dir_open(root, 0, NULL);

            if (dir == NULL)
                continue;

            while ((name = g_dir_read_name(dir)) != NULL) {
                g_autofree gchar *path = g_build_filename(root, name, NULL);
                GStatBuf info;
                gsize i;

                if (!g_file_test(path, G_FILE_TEST_IS_DIR))
                    continue;

                /*
                 * Already put aside once.  Listing it again would ask
                 * the same question a second time, which is the one
                 * thing "forget" was supposed to stop.
                 */
                if (g_str_has_suffix(name, ".discarded"))
                    continue;

                if (clawt_agent_manager_get(self->agents, name) != NULL)
                    continue;

                if (g_hash_table_contains(seen, name))
                    continue;

                json_builder_begin_object(builder);
                json_builder_set_member_name(builder, "id");
                json_builder_add_string_value(builder, name);
                json_builder_set_member_name(builder, "path");
                json_builder_add_string_value(builder, path);
                json_builder_set_member_name(builder, "kind");
                json_builder_add_string_value(builder,
                                              d == 0 ? "state" : "workspace");

                /*
                 * What is actually in there, so a person can tell a
                 * real agent's leftovers from an empty directory
                 * somebody made by hand.
                 */
                json_builder_set_member_name(builder, "holds");
                json_builder_begin_array(builder);

                for (i = 0; interesting[i] != NULL; i++) {
                    g_autofree gchar *file =
                        g_build_filename(path, interesting[i], NULL);

                    if (g_file_test(file, G_FILE_TEST_EXISTS))
                        json_builder_add_string_value(builder, interesting[i]);
                }

                json_builder_end_array(builder);

                json_builder_set_member_name(builder, "modified");
                json_builder_add_int_value(
                    builder, g_stat(path, &info) == 0 ? info.st_mtime : 0);

                json_builder_end_object(builder);

                g_hash_table_add(seen, g_strdup(name));
            }
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "agent.import") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "id");
        const gchar *from = clawt_ipc_payload_string(payload, "from");
        const gchar *mode_nick = clawt_ipc_payload_string(payload, "mode");
        gboolean keep_git = clawt_ipc_payload_boolean(payload, "keep_git",
                                                       FALSE);
        ClawtImportMode mode = CLAWT_IMPORT_COPY;
        g_autofree gchar *source = NULL;
        g_autofree gchar *workspace = NULL;
        g_autofree gchar *detail = NULL;
        ClawtAgentConfig *created;
        guint copied = 0;

        if (agent_id == NULL || from == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "id and from are both required");

        /*
         * Refused rather than quietly defaulted. An unrecognised mode
         * would otherwise become a copy, so somebody who typed `--lnik`
         * would get a fork of their workspace instead of a link to it
         * and find out only when their edits stopped reaching the agent.
         */
        if (mode_nick != NULL &&
            g_strcmp0(mode_nick,
                      clawt_import_mode_nth_nick(
                          clawt_import_mode_from_nick(mode_nick))) != 0)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "mode must be copy, link or git");

        mode = clawt_import_mode_from_nick(mode_nick);

        /*
         * A git import names a URL rather than a directory, so the
         * directory check belongs to the two modes that take one --
         * clawt_workspace_adopt() makes it, where it can say which kind
         * of thing was expected.
         */
        source = g_strdup(from);

        if (clawt_agent_manager_get(self->agents, agent_id) != NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_ALREADY_EXISTS,
                                       "there is already an agent with that "
                                       "id");

        /*
         * The config entry first, so the workspace path is whatever
         * clawtilla would have chosen -- an import is an agent like any
         * other afterwards, not one that remembers where it came from.
         */
        created = clawt_config_add_agent(self->config, agent_id, &error);

        if (created == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        workspace = clawt_agent_config_get_workspace(created);

        if (!clawt_workspace_adopt(mode, source, workspace, keep_git,
                                   &copied, &detail, &error)) {
            clawt_config_remove_agent(self->config, agent_id);
            return clawt_ipc_error_new(request, error->code, error->message);
        }

        /*
         * Anything the source said about itself that clawtilla owns
         * too. A standalone libreclaw instance keeps its provider and
         * model in its own config.yaml, and an import that dropped them
         * would quietly move the agent onto the fleet defaults.
         */
        {
            g_autofree gchar *imported = g_build_filename(workspace,
                                                          "config.yaml", NULL);

            clawt_config_adopt_libreclaw(created, imported);
        }

        /*
         * And the persona it already had.
         *
         * clawtilla names its identity files in org, a workspace from
         * anywhere else names them in markdown, and the two sets do not
         * collide -- so the copy above brought a complete persona across
         * and the scaffolder then wrote a blank .org beside every file
         * of it. The agent loaded the blanks: an import that succeeded,
         * reported every file copied, and produced something wearing the
         * right name with "/(fill in)/" where its character should be.
         *
         * Only when nothing was configured. An id and a persona given on
         * the import frame are the caller's, not ours to overrule.
         */
        if (!clawt_agent_config_has_key(created, "persona.identity_files")) {
            g_auto(GStrv) adopted =
                clawt_workspace_detect_identity_files(workspace);

            if (adopted != NULL && adopted[0] != NULL) {
                g_autofree gchar *joined = g_strjoinv(", ", adopted);

                clawt_agent_config_set_string_list(
                    created, "persona.identity_files",
                    (const gchar *const *)adopted);

                g_message("import: '%s' already had a persona; loading %s "
                          "rather than scaffolding blanks beside it",
                          agent_id, joined);
            }
        }

        if (!clawt_config_save(self->config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        /*
         * The same two steps agent.create takes.  Saving the config is
         * not enough to make an agent exist: the manager builds its
         * agents from a reloaded config, and without this the import
         * succeeded, wrote everything correctly, and then did not
         * appear in `agent list`.
         */
        if (!clawt_daemon_reload(self, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        clawt_agent_manager_load(self->agents, NULL);
        clawt_event_bus_emit(self->bus, "agent.created", agent_id);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "id");
        json_builder_add_string_value(builder, agent_id);
        json_builder_set_member_name(builder, "workspace");
        json_builder_add_string_value(builder, workspace);
        json_builder_set_member_name(builder, "files");
        json_builder_add_int_value(builder, copied);
        json_builder_set_member_name(builder, "mode");
        json_builder_add_string_value(builder,
                                      clawt_import_mode_nth_nick(mode));

        /*
         * What actually happened, in a sentence.
         *
         * Two of the three modes have an outcome the client could not
         * predict -- a git import is a submodule only where the
         * workspace root is inside a repository -- and the difference
         * between a workspace somebody's `git status` tracks and one it
         * does not is worth saying rather than leaving to be discovered.
         */
        if (detail != NULL) {
            json_builder_set_member_name(builder, "detail");
            json_builder_add_string_value(builder, detail);
        }

        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "agent.forget") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "id");
        g_autofree gchar *state_path = NULL;
        g_autofree gchar *workspace_root = NULL;
        g_autofree gchar *workspace_path = NULL;

        if (agent_id == NULL || strchr(agent_id, '/') != NULL ||
            g_strcmp0(agent_id, "..") == 0)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "not a plain agent id");

        if (clawt_agent_manager_get(self->agents, agent_id) != NULL)
            return clawt_ipc_error_new(
                request, CLAWT_ERROR_AGENT_STATE,
                "that agent is in the config; remove it with agent.remove");

        state_path = g_build_filename(self->state_dir, "agents", agent_id,
                                      NULL);
        workspace_root = clawt_config_get_path_value(
            self->config, "defaults.workspace_root");

        if (workspace_root != NULL)
            workspace_path = g_build_filename(workspace_root, agent_id, NULL);

        /*
         * Moved aside rather than deleted.  This is somebody's agent --
         * its transcripts, its memories, whatever it was told about
         * them -- and "I never created this" is a thing people say
         * about directories they later want back.
         */
        {
            g_autoptr(GString) moved = g_string_new(NULL);
            const gchar *paths[] = { state_path, workspace_path };
            gsize i;

            for (i = 0; i < G_N_ELEMENTS(paths); i++) {
                g_autofree gchar *aside = NULL;

                if (paths[i] == NULL ||
                    !g_file_test(paths[i], G_FILE_TEST_IS_DIR))
                    continue;

                aside = g_strconcat(paths[i], ".discarded", NULL);

                if (g_rename(paths[i], aside) == 0) {
                    if (moved->len > 0)
                        g_string_append(moved, ", ");

                    g_string_append(moved, aside);
                }
            }

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "moved");
            json_builder_add_string_value(builder, moved->str);
            json_builder_end_object(builder);
        }

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "agent.create") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "id");
        g_autoptr(GHashTable) fields = NULL;
        ClawtAgentConfig *created;

        if (agent_id == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "an agent needs an id");

        /*
         * The frame's vocabulary, translated into configuration keys
         * here so the shared implementation never has to know it.
         */
        {
            static const struct {
                const gchar *from;
                const gchar *to;
            } names[] = {
                { "name",           "name" },
                { "description",    "description" },
                { "model",          "model.model" },
                { "provider",       "model.provider" },
                { "computer",       "computer.type" },
                { "confine",        "computer.host.confine" },
                { "image",          "computer.container.image" },
                { "vm_image",       "computer.vm.image" },
                { "vm_cpus",        "computer.vm.cpus" },
                { "vm_memory_mb",   "computer.vm.memory_mb" },
                { "vm_disk_gb",     "computer.vm.disk_gb" },
                { "vm_resolution",  "computer.vm.resolution" },
                { "team",           "team" },
                { "team_role",      "team_role" },
                { "workspace",      "workspace" },
                { NULL, NULL }
            };
            gsize i;

            fields = g_hash_table_new(g_str_hash, g_str_equal);

            for (i = 0; names[i].from != NULL; i++) {
                const gchar *value = clawt_ipc_payload_string(payload,
                                                              names[i].from);

                if (value != NULL)
                    g_hash_table_insert(fields, (gpointer)names[i].to,
                                        (gpointer)value);
            }
        }

        created = clawt_daemon_create_agent(self, agent_id, fields, NULL, NULL,
                                            &error);

        if (created == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "id");
        json_builder_add_string_value(builder, agent_id);

        /*
         * ...and started, because creating an agent and building the
         * thing it works in were two steps and only one of them had a
         * button.
         *
         * A computer is built at *start*, not at create: a VM agent
         * created and left alone has a config file and no machine.
         * `defaults.autostart` does not cover it either -- it is false
         * by default and means "comes back with the daemon", which is a
         * different question from whether the thing somebody just asked
         * for exists.
         */
        if (clawt_ipc_payload_boolean(payload, "start", TRUE)) {
            g_autoptr(GError) start_error = NULL;
            gboolean started = clawt_daemon_start_agent(self, agent_id,
                                                        &start_error);

            json_builder_set_member_name(builder, "started");
            json_builder_add_boolean_value(builder, started);

            /*
             * Reported, never fatal. The agent exists and its
             * configuration is on disk; rolling that back because a
             * hypervisor was busy would throw away everything the person
             * had just typed.
             */
            if (!started && start_error != NULL) {
                json_builder_set_member_name(builder, "start_error");
                json_builder_add_string_value(builder, start_error->message);
            }
        }

        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "agent.remove") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        gboolean with_computer =
            clawt_ipc_payload_boolean(payload, "remove_computer", FALSE);
        g_autofree gchar *computer_detail = NULL;
        g_autofree gchar *files_detail = NULL;
        gboolean linked_workspace = FALSE;

        if (agent_id == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "which agent?");

        /*
         * The container or VM, torn down before the agent goes.
         *
         * Removing an agent used to leave its computer running, with a
         * name derived from an agent that no longer existed -- so the
         * only way to find it again was to remember what it had been
         * called. Opt-in, because a container may hold work that was
         * never anywhere else.
         *
         * Done before the config entry is dropped: the computer is built
         * from that config, and afterwards there is nothing left to
         * build it from.
         */
        if (with_computer) {
            ClawtAgent *agent = clawt_agent_manager_get(self->agents,
                                                         agent_id);
            ClawtComputer *computer = (agent != NULL)
                                      ? clawt_agent_get_computer(agent)
                                      : NULL;
            g_autoptr(ClawtComputer) built = NULL;

            /*
             * An agent that was never started has no computer object,
             * but its container may still be there from a previous run.
             * Building one from the config finds it by name.
             */
            if (computer == NULL) {
                ClawtAgentConfig *agent_config =
                    clawt_config_get_agent(self->config, agent_id);

                if (agent_config != NULL) {
                    g_autoptr(GPtrArray) defaults =
                        clawt_config_get_default_mounts(self->config);

                    built = clawt_computer_factory_create(agent_config,
                                                          defaults,
                                                          self->pod_bridge,
                                                          NULL);
                    computer = built;
                }
            }

            if (computer != NULL &&
                clawt_computer_get_computer_type(computer) !=
                    CLAWT_COMPUTER_NONE) {
                g_autoptr(GError) teardown_error = NULL;

                if (clawt_computer_teardown(computer, &teardown_error)) {
                    computer_detail = g_strdup("removed");
                } else {
                    /*
                     * Reported, not fatal.  The agent is still going, and
                     * refusing to remove it because its container had
                     * already been deleted by hand would be absurd.
                     */
                    computer_detail = g_strdup(
                        teardown_error != NULL ? teardown_error->message
                                               : "could not be removed");
                    g_warning("agent %s: computer not removed: %s", agent_id,
                              computer_detail);
                }
            }
        }

        clawt_daemon_stop_agent(self, agent_id, FALSE);

        /*
         * The files, before the config entry goes: every path is
         * derived from that entry, and afterwards there is nothing left
         * to derive them from.
         */
        if (clawt_ipc_payload_boolean(payload, "remove_files", FALSE)) {
            ClawtAgentConfig *doomed = clawt_config_get_agent(self->config,
                                                              agent_id);
            g_autoptr(GError) purge_error = NULL;
            gboolean was_linked = FALSE;

            /*
             * Reported beside `files` rather than instead of it. `files`
             * is the success sentinel every client already branches on,
             * and a linked workspace genuinely was removed -- what
             * differs is what survived, which is a sentence rather than
             * an outcome.
             */
            if (doomed != NULL &&
                !clawt_daemon_purge_agent_files(self, doomed, &was_linked,
                                                &purge_error))
                files_detail = g_strdup(purge_error->message);
            else if (doomed != NULL)
                files_detail = g_strdup("removed");

            linked_workspace = was_linked;
        }

        if (!clawt_config_remove_agent(self->config, agent_id))
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        if (!clawt_config_save(self->config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        /*
         * Without remove_files the agent's state directory -- its
         * mailbox, its transcripts, its rendered config -- is left on
         * disk. Removing an agent from the fleet is reversible; deleting
         * its history is not, so it is asked for rather than assumed.
         */
        clawt_agent_manager_load(self->agents, NULL);
        clawt_event_bus_emit(self->bus, "agent.removed", agent_id);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "id");
        json_builder_add_string_value(builder, agent_id);

        /* What happened to the computer, so a client can say so. */
        if (computer_detail != NULL) {
            json_builder_set_member_name(builder, "computer");
            json_builder_add_string_value(builder, computer_detail);
        }

        /* ...and to the files, which is the irreversible half. */
        if (files_detail != NULL) {
            json_builder_set_member_name(builder, "files");
            json_builder_add_string_value(builder, files_detail);
        }

        if (linked_workspace) {
            json_builder_set_member_name(builder, "linked_workspace");
            json_builder_add_boolean_value(builder, TRUE);
        }

        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "agent.reorder") == 0) {
        const gchar *ids = clawt_ipc_payload_string(payload, "agents");
        g_auto(GStrv) wanted = NULL;
        gsize i;

        if (ids == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "agents is required: the ids in the "
                                       "order you want them, comma "
                                       "separated");

        wanted = g_strsplit(ids, ",", -1);

        /*
         * Numbered from one, in steps of ten.
         *
         * The gap is not decoration: it leaves room to place one agent
         * between two others by setting a single number by hand, which
         * is the only way to do it in a text editor without renumbering
         * the whole file.
         */
        for (i = 0; wanted[i] != NULL; i++) {
            const gchar *agent_id = g_strstrip(wanted[i]);
            ClawtAgentConfig *config;
            g_autofree gchar *position = NULL;

            if (*agent_id == '\0')
                continue;

            config = clawt_config_get_agent(self->config, agent_id);

            /*
             * An id that is not there is skipped rather than refused.
             * The list comes from a client's view of the fleet, which
             * may be a moment behind one that has just been removed --
             * and failing the whole reorder over that would lose the
             * arrangement somebody had just made.
             */
            if (config == NULL)
                continue;

            position = g_strdup_printf("%u", (guint)((i + 1) * 10));
            clawt_agent_config_set_string(config, "order", position);
        }

        if (!clawt_config_save(self->config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        clawt_event_bus_emit(self->bus, "agent.changed", NULL);

        return clawt_ipc_response_new(request, NULL);
    }

    if (g_strcmp0(kind, "agent.set") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        const gchar *key = clawt_ipc_payload_string(payload, "key");
        const gchar *value = clawt_ipc_payload_string(payload, "value");
        ClawtAgentConfig *config;

        if (agent_id == NULL || key == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "agent and key are both required");

        config = clawt_config_get_agent(self->config, agent_id);

        if (config == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        /*
         * An enum is checked against the schema before it is written.
         *
         * Nothing did, so `computer.type teleporter` was accepted here,
         * echoed back as saved, and written to clawtilla.yaml. Every
         * reader of it then fell back to the schema default, so the
         * agent went on with the computer it already had while the file
         * said otherwise -- and at the next daemon load the validator
         * caught what this did not and turned the agent into a shadow,
         * a long way from the command that caused it.
         *
         * The allowed values come off the enum's own #GType through
         * clawt_enum_nick_list(), for the same reason the type comes off
         * the schema: a set of values spelled out beside the refusal is
         * the enum written a second time, and the second copy stops
         * being true in silence.
         */
        {
            const ClawtSchemaEntry *entry =
                clawt_daemon_agent_setting_entry(key);
            gint parsed = 0;

            /*
             * An empty value is refused with the rest, deliberately.
             * There is no "unset" here, so clearing an enum writes
             * `key: ""` -- and the loader reads that as an unknown
             * nickname and shadows the agent, which is the very failure
             * this check exists to prevent.
             */
            if (entry != NULL && entry->type == CLAWT_SCHEMA_ENUM &&
                value != NULL &&
                !clawt_enum_from_nick(entry->enum_type(), value, &parsed)) {
                g_autofree gchar *allowed =
                    clawt_enum_nick_list(entry->enum_type());
                g_autofree gchar *message = g_strdup_printf(
                    "'%s' is not a value for %s: it takes one of %s",
                    value, key, allowed);

                return clawt_ipc_error_new(request,
                                           CLAWT_ERROR_INVALID_ARGUMENT,
                                           message);
            }
        }

        /*
         * Dispatch on what the schema says the key *is*, rather than
         * writing every value as a scalar.
         *
         * A STRING_LIST written as a scalar is not merely ugly: the
         * reader refuses anything that is not a YAML sequence, so the
         * value was accepted here, echoed back to the client, saved to
         * clawtilla.yaml, and then read back as the schema default. The
         * one that exposed it was persona.identity_files -- an agent
         * pointed at its real persona files went on loading the seven
         * generated .org stubs, and every surface agreed the setting
         * had been saved.
         */
        clawt_agent_config_set_from_string(config, key, value);

        if (!clawt_config_save(self->config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        /*
         * ...and the agent's own files, which are derived from what was
         * just changed.
         *
         * Saving used to be the whole of it, so a setting was written to
         * clawtilla.yaml and nothing the agent reads was touched. The
         * one that made that visible was tools.manage_fleet: the gate
         * answers from the live config and was right immediately, while
         * TOOLS.org went on listing the tools as they were at the last
         * daemon start. Two answers to "what do I have", and the file is
         * the one that reaches the agent's prompt.
         */
        {
            g_autoptr(GPtrArray) refusals = clawt_daemon_render_refusals_new();

            clawt_daemon_render_all_agents_into(self, refusals);
            clawt_event_bus_emit(self->bus, "agent.changed", agent_id);

            json_builder_begin_object(builder);
            clawt_daemon_add_render_refusals(builder, refusals);
        }

        json_builder_set_member_name(builder, "agent");
        json_builder_add_string_value(builder, agent_id);

        /*
         * The shadow decision is retaken here, not left to the next load.
         *
         * It is made once, when the config is parsed -- so setting the
         * very key an agent was disabled for wrote the value, answered
         * with the key and its new value, and left the agent shadowed
         * with the old reason.  `agent list` still said `shadow`, which
         * reads as the setting not having worked.  The only remedy was
         * restarting the daemon, and on a remote one there was no way to
         * ask for that at all.
         *
         * Reported either way: still refusing is the interesting answer,
         * and a client that only hears "saved" cannot tell the two apart.
         */
        {
            ClawtAgentConfig *agent_config =
                clawt_config_get_agent(self->config, agent_id);
            gboolean usable = TRUE;

            if (agent_config != NULL) {
                ClawtAgent *agent =
                    clawt_agent_manager_get(self->agents, agent_id);

                usable = clawt_agent_config_revalidate(agent_config);

                /*
                 * And the agent, which keeps its own state.  Revalidating
                 * only the config left `agent list` reporting the old
                 * answer -- the shadow decision reaches a client through
                 * ClawtAgent, not through ClawtAgentConfig.
                 */
                if (agent != NULL)
                    clawt_agent_revalidate(agent);
            }

            json_builder_set_member_name(builder, "shadow");
            json_builder_add_boolean_value(builder, !usable);

            if (!usable && agent_config != NULL) {
                json_builder_set_member_name(builder, "shadow_reason");
                json_builder_add_string_value(
                    builder,
                    clawt_agent_config_get_shadow_reason(agent_config));
            }
        }

        /*
         * Which keys those are is
         * clawt_daemon_setting_needs_a_new_session()'s to say; only a
         * *running* agent is told, because a stopped one will start a
         * fresh session anyway and telling it to restart would be advice
         * about nothing.
         */
        json_builder_set_member_name(builder, "restart_required");
        json_builder_add_boolean_value(
            builder, clawt_daemon_setting_needs_a_new_session(key) &&
                     clawt_agent_get_state(
                         clawt_agent_manager_get(self->agents, agent_id)) ==
                     CLAWT_AGENT_STATE_RUNNING);

        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    *handled = FALSE;
    return NULL;
}
