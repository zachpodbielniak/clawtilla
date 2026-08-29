/*
 * daemon-design.c - The client surface: design.*
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
clawt_daemon_handle_design(
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

    if (g_strcmp0(kind, "design.agent") == 0) {
        /*
         * The questionnaire.
         *
         * One free-text box asked the person to write a paragraph that
         * happened to contain everything the model needed, and a
         * paragraph that leaves out the boundaries produces an agent
         * with none.  Named questions ask for each thing once, and an
         * unanswered one is visibly unanswered rather than silently
         * absent.
         */
        static const struct {
            const gchar *field;
            const gchar *question;
        } questions[] = {
            { "purpose",     "What should this agent do?" },
            { "boundaries",  "What should it never do?" },
            { "needs",       "What does it need to work on: files, "
                             "commands, the network, nothing?" },
            { "personality", "How should it come across?" },
            { "projects",    "What is it working on, and where does that "
                             "live?" },
            { "notes",       "Anything else it should know?" },
            { NULL, NULL }
        };
        const gchar *description = clawt_ipc_payload_string(payload,
                                                            "description");
        g_autoptr(GString) brief = g_string_new(NULL);
        g_autoptr(ClawtAgentDesigner) designer = NULL;
        g_autofree gchar *preview = NULL;
        g_autofree gchar *draft_id = NULL;
        GHashTable *draft;
        gsize i;

        for (i = 0; questions[i].field != NULL; i++) {
            const gchar *answer = clawt_ipc_payload_string(payload,
                                                            questions[i].field);

            if (answer == NULL || *answer == '\0')
                continue;

            g_string_append_printf(brief, "%s\n%s\n\n",
                                   questions[i].question, answer);
        }

        /*
         * The old single-field form still works.  The CLI takes a
         * sentence, and a client that has not been updated should keep
         * designing agents rather than start failing.
         */
        if (brief->len == 0 && description != NULL && *description != '\0')
            g_string_append(brief, description);

        if (brief->len == 0)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "answer at least one question, or "
                                       "send a description");

        designer = clawt_agent_designer_new(self->config);

        /*
         * An id or name the person typed is theirs.  Models rename
         * routinely -- to something they consider more descriptive --
         * and the agent then appears under a name nobody chose, with
         * any script that asked for the original looking at the wrong
         * agent.
         */
        clawt_agent_designer_pin_identity(
            designer, clawt_ipc_payload_string(payload, "id"),
            clawt_ipc_payload_string(payload, "name"));

        /*
         * And so is the computer, for a sharper reason than the name.
         * The designer cannot name a disk image, so a VM it chose by
         * itself never provisions -- it refuses naming computer.vm.image,
         * a setting nothing in the design ever set. The client collects
         * that above the Design button; this is where it arrives.
         */
        if (clawt_ipc_payload_string(payload, "computer") != NULL) {
            g_autoptr(GHashTable) settings =
                g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                      g_free);
            static const struct {
                const gchar *member;
                const gchar *key;
            } carried[] = {
                { "image",     "computer.container.image" },
                { "vm_image",  "computer.vm.image" },
                { "vm_cpus",   "computer.vm.cpus" },
                { "vm_memory", "computer.vm.memory_mb" },
                { "vm_disk",   "computer.vm.disk_gb" },
                /*
                 * The team is a choice made on the form, and the model
                 * has no way to know which teams exist -- so it is
                 * carried through rather than left for the designer to
                 * guess at, the same as the disk image.
                 */
                { "team",      "team" },
                { NULL, NULL }
            };
            gsize c;

            for (c = 0; carried[c].member != NULL; c++) {
                const gchar *value =
                    clawt_ipc_payload_string(payload, carried[c].member);

                if (value != NULL && *value != '\0')
                    g_hash_table_insert(settings, g_strdup(carried[c].key),
                                        g_strdup(value));
            }

            clawt_agent_designer_pin_computer(
                designer, clawt_ipc_payload_string(payload, "computer"),
                settings);
        }

        /*
         * The model that designs is chosen per request, falling back to
         * ai_assist.  The one that drafts an agent and the one that then
         * runs it have no reason to be the same: a person will often
         * want their best model for the first and a cheap one for the
         * second.
         */
        if (clawt_ipc_payload_string(payload, "provider") != NULL) {
            if (!clawt_config_get_boolean(self->config, "ai_assist.enabled"))
                return clawt_ipc_error_new(
                    request, CLAWT_ERROR_NOT_SUPPORTED,
                    "AI-assisted agent creation is turned off; set "
                    "ai_assist.enabled: true");

            if (!clawt_agent_designer_set_provider_by_name(
                    designer,
                    clawt_ipc_payload_string(payload, "provider"),
                    clawt_ipc_payload_string(payload, "model"), &error))
                return clawt_ipc_error_new(request, error->code,
                                           error->message);
        } else if (!clawt_agent_designer_use_configured_provider(designer,
                                                                 &error)) {
            return clawt_ipc_error_new(request, error->code, error->message);
        }

        draft = clawt_agent_designer_design(designer, brief->str, NULL,
                                            &error);

        if (draft == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        preview = clawt_agent_designer_preview(designer);

        /*
         * Kept so design.commit creates exactly what was reviewed.
         * Bounded, because a client that designs and walks away should
         * not grow the daemon without limit.
         */
        draft_id = clawt_generate_token(NULL);

        if (draft_id == NULL)
            draft_id = g_strdup(g_hash_table_lookup(draft, "id"));

        if (g_hash_table_size(self->drafts) >= MAX_PENDING_DRAFTS) {
            GHashTableIter iter;
            gpointer oldest = NULL;

            g_hash_table_iter_init(&iter, self->drafts);

            if (g_hash_table_iter_next(&iter, &oldest, NULL))
                g_hash_table_remove(self->drafts, oldest);
        }

        g_hash_table_insert(self->drafts, g_strdup(draft_id),
                            g_object_ref(designer));

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "draft");
        json_builder_add_string_value(builder, draft_id);
        json_builder_set_member_name(builder, "yaml");
        json_builder_add_string_value(builder, preview);
        json_builder_set_member_name(builder, "id");
        json_builder_add_string_value(builder,
                                      g_hash_table_lookup(draft, "id"));

        /* The org files the model wrote, so a client can show them. */
        {
            GHashTable *files = clawt_agent_designer_get_files(designer);
            g_autoptr(GList) names = g_hash_table_get_keys(files);
            GList *f;

            names = g_list_sort(names, (GCompareFunc)g_strcmp0);

            json_builder_set_member_name(builder, "files");
            json_builder_begin_array(builder);

            for (f = names; f != NULL; f = f->next) {
                json_builder_begin_object(builder);
                json_builder_set_member_name(builder, "name");
                json_builder_add_string_value(builder, f->data);
                json_builder_set_member_name(builder, "content");
                json_builder_add_string_value(
                    builder, g_hash_table_lookup(files, f->data));
                json_builder_end_object(builder);
            }

            json_builder_end_array(builder);
        }

        json_builder_set_member_name(builder, "committed");
        json_builder_add_boolean_value(builder, FALSE);
        json_builder_set_member_name(builder, "notes");
        json_builder_add_string_value(
            builder, clawt_agent_designer_get_transcript(designer));
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "design.commit") == 0) {
        const gchar *draft_id = clawt_ipc_payload_string(payload, "draft");
        ClawtAgentDesigner *designer = (draft_id != NULL)
            ? g_hash_table_lookup(self->drafts, draft_id) : NULL;
        ClawtAgentConfig *created;

        /*
         * Creates the design that was reviewed, rather than asking the
         * model again.  A second run is a fresh conversation and would
         * produce something else -- which makes the preview a
         * demonstration rather than a decision.
         */
        if (designer == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such draft; design it again");

        created = clawt_agent_designer_commit(designer, &error);

        if (created == NULL) {
            g_hash_table_remove(self->drafts, draft_id);
            return clawt_ipc_error_new(request, error->code, error->message);
        }

        if (!clawt_config_save(self->config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        clawt_agent_manager_load(self->agents, NULL);

        {
            g_autoptr(GPtrArray) refusals = clawt_daemon_render_refusals_new();

            clawt_daemon_render_all_agents_into(self, refusals);

            json_builder_begin_object(builder);
            clawt_daemon_add_render_refusals(builder, refusals);
        }

        json_builder_set_member_name(builder, "id");
        json_builder_add_string_value(builder,
                                      clawt_agent_config_get_id(created));
        json_builder_set_member_name(builder, "committed");
        json_builder_add_boolean_value(builder, TRUE);

        /*
         * The same start agent.create does, and for the same reason: an
         * agent designed and committed is an agent somebody wanted. The
         * designer's own comment says it commits "the same path as
         * creating an agent by hand", which was true of the config call
         * and had already stopped being true of the validation around
         * it once before.
         */
        if (clawt_ipc_payload_boolean(payload, "start", TRUE)) {
            const gchar *created_id = clawt_agent_config_get_id(created);
            g_autoptr(GError) start_error = NULL;
            gboolean started = clawt_daemon_start_agent(self, created_id,
                                                        &start_error);

            json_builder_set_member_name(builder, "started");
            json_builder_add_boolean_value(builder, started);

            if (!started && start_error != NULL) {
                json_builder_set_member_name(builder, "start_error");
                json_builder_add_string_value(builder, start_error->message);
            }
        }

        json_builder_end_object(builder);

        g_hash_table_remove(self->drafts, draft_id);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "design.discard") == 0) {
        const gchar *draft_id = clawt_ipc_payload_string(payload, "draft");

        if (draft_id != NULL)
            g_hash_table_remove(self->drafts, draft_id);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "discarded");
        json_builder_add_boolean_value(builder, TRUE);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    *handled = FALSE;
    return NULL;
}
