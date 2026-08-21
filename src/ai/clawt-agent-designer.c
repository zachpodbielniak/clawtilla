/*
 * clawt-agent-designer.c - Designing an agent by describing it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "ai/clawt-agent-designer.h"

#include <string.h>

struct _ClawtAgentDesigner {
    GObject parent_instance;

    ClawtConfig     *config;
    AiProvider      *provider;
    AiToolExecutor  *executor;
    GHashTable      *draft;      /* config key -> value */
    gchar           *transcript;
    guint            max_turns;
    gboolean         committed;
};

G_DEFINE_FINAL_TYPE(ClawtAgentDesigner, clawt_agent_designer, G_TYPE_OBJECT)

enum {
    SIGNAL_DRAFT_CHANGED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

static void register_tools(ClawtAgentDesigner *self);

ClawtAgentDesigner *
clawt_agent_designer_new(ClawtConfig *config)
{
    ClawtAgentDesigner *self = g_object_new(CLAWT_TYPE_AGENT_DESIGNER, NULL);

    g_return_val_if_fail(CLAWT_IS_CONFIG(config), NULL);

    self->config = g_object_ref(config);

    /*
     * new_empty(), emphatically not new().
     *
     * ai_tool_executor_new() pre-registers bash, read, write and edit, and
     * they cannot be taken back.  A designer that is meant to fill in a
     * YAML block does not need a shell, and handing one to a model that
     * only had to pick a model name is how a helpful feature becomes an
     * incident.
     */
    self->executor = ai_tool_executor_new_empty();
    register_tools(self);

    return self;
}

void
clawt_agent_designer_set_provider(ClawtAgentDesigner *self,
                                  AiProvider *provider)
{
    g_return_if_fail(CLAWT_IS_AGENT_DESIGNER(self));

    g_clear_object(&self->provider);

    if (provider != NULL)
        self->provider = g_object_ref(provider);
}

gboolean
clawt_agent_designer_use_configured_provider(ClawtAgentDesigner  *self,
                                             GError             **error)
{
    g_autoptr(AiConfig) ai_config = NULL;
    const gchar *provider_name;
    const gchar *model;
    GObject *provider;

    g_return_val_if_fail(CLAWT_IS_AGENT_DESIGNER(self), FALSE);

    if (!clawt_config_get_boolean(self->config, "ai_assist.enabled")) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                            "AI-assisted agent creation is turned off; set "
                            "ai_assist.enabled: true");
        return FALSE;
    }

    provider_name = clawt_config_get_string(self->config,
                                            "ai_assist.provider");
    model = clawt_config_get_string(self->config, "ai_assist.model");

    ai_config = ai_config_new();

    if (model != NULL)
        ai_config_set_default_model(ai_config, model);

    provider = ai_provider_factory_new_from_string(
        provider_name != NULL ? provider_name : "claude-code", ai_config,
        error);

    if (provider == NULL) {
        /*
         * Named, because the likely cause is a provider spelled wrong in
         * the config rather than anything broken.
         */
        g_prefix_error(error, "ai_assist.provider '%s': ",
                       provider_name != NULL ? provider_name
                                             : "claude-code");
        return FALSE;
    }

    clawt_agent_designer_set_provider(self, AI_PROVIDER(provider));
    g_object_unref(provider);

    self->max_turns = (guint)clawt_config_get_int(self->config,
                                                  "ai_assist.max_turns");

    return TRUE;
}

void
clawt_agent_designer_set_max_turns(ClawtAgentDesigner *self, guint max_turns)
{
    g_return_if_fail(CLAWT_IS_AGENT_DESIGNER(self));

    self->max_turns = max_turns;
}

GHashTable *
clawt_agent_designer_get_draft(ClawtAgentDesigner *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT_DESIGNER(self), NULL);

    return self->draft;
}

const gchar *
clawt_agent_designer_get_transcript(ClawtAgentDesigner *self)
{
    g_return_val_if_fail(CLAWT_IS_AGENT_DESIGNER(self), NULL);

    return self->transcript;
}

/* ── The tools the model gets ────────────────────────────────────── */

static void
set_draft(ClawtAgentDesigner *self, const gchar *key, const gchar *value)
{
    if (key == NULL || value == NULL)
        return;

    g_hash_table_replace(self->draft, g_strdup(key), g_strdup(value));
    g_signal_emit(self, signals[SIGNAL_DRAFT_CHANGED], 0);
}

static const gchar *
input_string(AiToolUse *use, const gchar *name)
{
    return ai_tool_use_get_input_string(use, name);
}

static gchar *
tool_set_identity(AiToolUse *use, GCancellable *cancellable, GError **error,
                  gpointer user_data)
{
    ClawtAgentDesigner *self = user_data;
    const gchar *id = input_string(use, "id");

    (void)cancellable;

    if (id == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "id is required");
        return NULL;
    }

    if (!clawt_is_valid_id(id)) {
        /*
         * Rejected with the rule stated, so the next attempt is right.  A
         * bare "invalid" makes a model guess, and it usually guesses the
         * same thing again.
         */
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "'%s' is not a usable id: use lowercase letters, "
                    "digits, '-' and '_' only", id);
        return NULL;
    }

    if (clawt_config_get_agent(self->config, id) != NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_ALREADY_EXISTS,
                    "there is already an agent called '%s'; pick another id",
                    id);
        return NULL;
    }

    set_draft(self, "id", id);
    set_draft(self, "name", input_string(use, "name"));
    set_draft(self, "description", input_string(use, "description"));
    set_draft(self, "persona.system_prompt",
              input_string(use, "system_prompt"));

    return g_strdup_printf("The agent is now '%s'.", id);
}

static gchar *
tool_set_model(AiToolUse *use, GCancellable *cancellable, GError **error,
               gpointer user_data)
{
    ClawtAgentDesigner *self = user_data;

    (void)cancellable;
    (void)error;

    set_draft(self, "model.provider", input_string(use, "provider"));
    set_draft(self, "model.model", input_string(use, "model"));
    set_draft(self, "model.effort", input_string(use, "effort"));

    return g_strdup("Model settings recorded.");
}

static gchar *
tool_set_computer(AiToolUse *use, GCancellable *cancellable, GError **error,
                  gpointer user_data)
{
    ClawtAgentDesigner *self = user_data;
    const gchar *type = input_string(use, "type");
    const gchar *confine = input_string(use, "confine");

    (void)cancellable;

    if (type == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "type is required");
        return NULL;
    }

    {
        gint parsed = 0;

        if (!clawt_enum_from_nick(CLAWT_TYPE_COMPUTER_TYPE, type, &parsed)) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                        "'%s' is not a computer type; use none, host, "
                        "container or vm", type);
            return NULL;
        }
    }

    set_draft(self, "computer.type", type);

    if (g_strcmp0(type, "host") == 0) {
        /*
         * Host access is never drafted unconfined, whatever the model
         * asked for.  A description like "give it full access to my
         * machine" should still produce something confined by default:
         * the person can lift it deliberately, having read what it means.
         */
        set_draft(self, "computer.host.confine",
                  confine != NULL && g_strcmp0(confine, "none") != 0
                      ? confine : "workspace");
        set_draft(self, "computer.host.confirm_host_control", "true");

        if (g_strcmp0(confine, "none") == 0)
            return g_strdup(
                "Recorded as a host computer, confined to a workspace. "
                "Unconfined host access cannot be set from here -- the "
                "person has to turn it on themselves, having read what it "
                "allows.");
    }

    if (input_string(use, "image") != NULL)
        set_draft(self, "computer.container.image",
                  input_string(use, "image"));

    return g_strdup_printf("Computer set to %s.", type);
}

static gchar *
tool_add_integration(AiToolUse *use, GCancellable *cancellable,
                     GError **error, gpointer user_data)
{
    ClawtAgentDesigner *self = user_data;
    const gchar *id = input_string(use, "integration");
    g_autofree gchar *key = NULL;

    (void)cancellable;

    if (clawt_integration_find(id) == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "'%s' is not an integration clawtilla knows about", id);
        return NULL;
    }

    key = g_strdup_printf("integrations.%s.enabled", id);
    set_draft(self, key, "true");

    /*
     * The model is told what is still missing rather than being allowed to
     * invent a homeserver.  Credentials and endpoints are the person's to
     * supply.
     */
    return g_strdup_printf(
        "%s enabled. Its settings and credentials still need filling in by "
        "hand.", id);
}

static gchar *
tool_set_tools(AiToolUse *use, GCancellable *cancellable, GError **error,
               gpointer user_data)
{
    ClawtAgentDesigner *self = user_data;

    (void)cancellable;
    (void)error;

    set_draft(self, "tools.allow", input_string(use, "allow"));
    set_draft(self, "tools.deny", input_string(use, "deny"));

    return g_strdup("Tool permissions recorded.");
}

static gchar *
tool_preview(AiToolUse *use, GCancellable *cancellable, GError **error,
             gpointer user_data)
{
    ClawtAgentDesigner *self = user_data;

    (void)use;
    (void)cancellable;
    (void)error;

    return clawt_agent_designer_preview(self);
}

static gchar *
tool_commit(AiToolUse *use, GCancellable *cancellable, GError **error,
            gpointer user_data)
{
    ClawtAgentDesigner *self = user_data;

    (void)use;
    (void)cancellable;

    if (g_hash_table_lookup(self->draft, "id") == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "call set_identity first: the agent has no id");
        return NULL;
    }

    /*
     * Marked, not written.  The design conversation ends here and a person
     * sees the result before it becomes an agent -- committing straight
     * from a tool call would mean the model's last word created something
     * nobody reviewed.
     */
    self->committed = TRUE;

    return g_strdup("Design finished. The draft is ready for review.");
}

static void
register_tool(ClawtAgentDesigner *self, const gchar *name,
              const gchar *description, AiToolCallback callback,
              const ClawtParamInfo *params, gsize n_params)
{
    g_autoptr(AiTool) tool = ai_tool_new(name, description);
    gsize i;

    for (i = 0; i < n_params; i++)
        ai_tool_add_parameter(tool, params[i].name, params[i].type_name,
                              params[i].description, params[i].required);

    ai_tool_executor_register_callback(self->executor, tool, callback, self,
                                       NULL);
}

static void
register_tools(ClawtAgentDesigner *self)
{
    static const ClawtParamInfo identity_params[] = {
        { "id", "string", "A short identifier: lowercase letters, digits, "
                          "'-' and '_'.", TRUE },
        { "name", "string", "The display name.", FALSE },
        { "description", "string", "One line on what this agent is for. "
                                   "Other agents read this when deciding "
                                   "who to delegate to.", FALSE },
        { "system_prompt", "string", "The agent's standing instructions.",
          FALSE }
    };

    static const ClawtParamInfo model_params[] = {
        { "provider", "string", "Which provider, e.g. claude-code.", FALSE },
        { "model", "string", "Which model, e.g. opus or sonnet.", FALSE },
        { "effort", "string", "low, medium, high, xhigh or max.", FALSE }
    };

    static const ClawtParamInfo computer_params[] = {
        { "type", "string", "none, host, container or vm.", TRUE },
        { "confine", "string", "For a host computer: workspace, allowlist "
                               "or bwrap.", FALSE },
        { "image", "string", "For a container: the image to run.", FALSE }
    };

    static const ClawtParamInfo integration_params[] = {
        { "integration", "string", "matrix, email, webhook, local or cmacs.",
          TRUE }
    };

    static const ClawtParamInfo tools_params[] = {
        { "allow", "string", "Comma-separated tool names this agent may use. "
                             "Omit to allow everything its capabilities "
                             "permit.", FALSE },
        { "deny", "string", "Comma-separated tool names to refuse.", FALSE }
    };

    static const ClawtParamInfo no_params[] = {
        { NULL, NULL, NULL, FALSE }
    };

    register_tool(self, "set_identity",
                  "Give the agent its id, name and purpose. Call this "
                  "first.",
                  tool_set_identity, identity_params,
                  G_N_ELEMENTS(identity_params));

    register_tool(self, "set_model",
                  "Choose the model this agent runs on.",
                  tool_set_model, model_params, G_N_ELEMENTS(model_params));

    register_tool(self, "set_computer",
                  "Give the agent a computer to work on, or none.",
                  tool_set_computer, computer_params,
                  G_N_ELEMENTS(computer_params));

    register_tool(self, "add_integration",
                  "Connect the agent to Matrix, email or a webhook.",
                  tool_add_integration, integration_params,
                  G_N_ELEMENTS(integration_params));

    register_tool(self, "set_tools",
                  "Narrow which orchestration tools this agent may use.",
                  tool_set_tools, tools_params, G_N_ELEMENTS(tools_params));

    register_tool(self, "preview",
                  "Show the configuration as it stands.",
                  tool_preview, no_params, 0);

    register_tool(self, "commit",
                  "Finish. Call this when the agent is fully described.",
                  tool_commit, no_params, 0);
}

/* ── Running the design ──────────────────────────────────────────── */

gchar *
clawt_agent_designer_preview(ClawtAgentDesigner *self)
{
    g_autoptr(GString) out = NULL;
    g_autoptr(GList) keys = NULL;
    const gchar *id;
    GList *l;

    g_return_val_if_fail(CLAWT_IS_AGENT_DESIGNER(self), NULL);

    id = g_hash_table_lookup(self->draft, "id");

    if (id == NULL)
        return g_strdup("Nothing has been drafted yet.");

    out = g_string_new("agents:\n");
    g_string_append_printf(out, "  - id: \"%s\"\n", id);

    keys = g_hash_table_get_keys(self->draft);
    keys = g_list_sort(keys, (GCompareFunc)g_strcmp0);

    for (l = keys; l != NULL; l = l->next) {
        const gchar *key = l->data;
        const gchar *value = g_hash_table_lookup(self->draft, key);

        if (g_strcmp0(key, "id") == 0)
            continue;

        /*
         * Dotted keys rather than nested blocks.  This is a preview meant
         * to be read, and the nesting is what makes a YAML diff hard to
         * scan; clawt_config_set_string() understands the dotted form on
         * the way in.
         */
        g_string_append_printf(out, "    %s: \"%s\"\n", key, value);
    }

    return g_string_free(g_steal_pointer(&out), FALSE);
}

GHashTable *
clawt_agent_designer_design(ClawtAgentDesigner *self,
                            const gchar *description,
                            GCancellable *cancellable, GError **error)
{
    g_autoptr(AiMessage) message = NULL;
    g_autofree gchar *reply = NULL;
    g_autofree gchar *system_prompt = NULL;
    GList *messages = NULL;

    g_return_val_if_fail(CLAWT_IS_AGENT_DESIGNER(self), NULL);
    g_return_val_if_fail(description != NULL, NULL);

    if (self->provider == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_AI,
                            "no AI provider is configured; set "
                            "ai_assist.provider in clawtilla.yaml");
        return NULL;
    }

    system_prompt = g_strdup_printf(
        "You are designing one agent for a clawtilla fleet.\n"
        "\n"
        "Use the tools to fill in the agent's configuration, then call "
        "commit. Do not ask questions -- work from what you are given and "
        "choose sensible defaults for the rest.\n"
        "\n"
        "Guidance:\n"
        "- Give the agent a description. Other agents read it when "
        "deciding who to delegate work to, so say what it is for rather "
        "than what it is called.\n"
        "- Only give it a computer if the work needs one. An agent that "
        "answers questions does not need a container.\n"
        "- Prefer a container to the host. Host access is for work that "
        "genuinely has to touch the real machine.\n"
        "- Do not invent credentials, hostnames or tokens. Enabling an "
        "integration is enough; the person fills in the rest.\n"
        "\n"
        "There %s already %u agent(s) in this fleet.",
        clawt_config_get_agents(self->config)->len == 1 ? "is" : "are",
        clawt_config_get_agents(self->config)->len);

    message = ai_message_new_user(description);
    messages = g_list_append(NULL, message);

    reply = ai_tool_executor_run(self->executor, self->provider, messages,
                                 system_prompt, 0, cancellable, error);

    g_list_free(messages);

    if (reply == NULL)
        return NULL;

    g_free(self->transcript);
    self->transcript = g_steal_pointer(&reply);

    if (g_hash_table_lookup(self->draft, "id") == NULL) {
        /*
         * A model that talked without ever calling a tool has produced
         * nothing usable.  Saying so is better than returning an empty
         * draft that fails confusingly at commit.
         */
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_AI,
                            "the model did not draft an agent: it never "
                            "called set_identity");
        return NULL;
    }

    return self->draft;
}

ClawtAgentConfig *
clawt_agent_designer_commit(ClawtAgentDesigner *self, GError **error)
{
    ClawtAgentConfig *agent;
    g_autoptr(GList) keys = NULL;
    const gchar *id;
    GList *l;

    g_return_val_if_fail(CLAWT_IS_AGENT_DESIGNER(self), NULL);

    id = g_hash_table_lookup(self->draft, "id");

    if (id == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "there is nothing to commit");
        return NULL;
    }

    /*
     * The same path as creating an agent by hand.  A separate write path
     * for AI-created agents would be a second implementation of the rules,
     * and the two would drift.
     */
    agent = clawt_config_add_agent(self->config, id, error);

    if (agent == NULL)
        return NULL;

    keys = g_hash_table_get_keys(self->draft);
    keys = g_list_sort(keys, (GCompareFunc)g_strcmp0);

    for (l = keys; l != NULL; l = l->next) {
        const gchar *key = l->data;

        if (g_strcmp0(key, "id") == 0)
            continue;

        clawt_agent_config_set_string(agent, key,
                                      g_hash_table_lookup(self->draft, key));
    }

    if (!clawt_integration_validate(agent, error)) {
        /*
         * Rolled back rather than left half-created.  An agent that exists
         * but cannot start is worse than one that was never added: it
         * shows up in every listing looking real.
         */
        clawt_config_remove_agent(self->config, id);
        return NULL;
    }

    return agent;
}

static void
clawt_agent_designer_dispose(GObject *object)
{
    ClawtAgentDesigner *self = CLAWT_AGENT_DESIGNER(object);

    g_clear_object(&self->executor);
    g_clear_object(&self->provider);
    g_clear_object(&self->config);

    G_OBJECT_CLASS(clawt_agent_designer_parent_class)->dispose(object);
}

static void
clawt_agent_designer_finalize(GObject *object)
{
    ClawtAgentDesigner *self = CLAWT_AGENT_DESIGNER(object);

    g_clear_pointer(&self->draft, g_hash_table_unref);
    g_free(self->transcript);

    G_OBJECT_CLASS(clawt_agent_designer_parent_class)->finalize(object);
}

static void
clawt_agent_designer_class_init(ClawtAgentDesignerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->dispose = clawt_agent_designer_dispose;
    object_class->finalize = clawt_agent_designer_finalize;

    /**
     * ClawtAgentDesigner::draft-changed:
     * @self: the designer
     *
     * Emitted whenever the model edits the draft, so a client can show
     * the design taking shape rather than a spinner.
     */
    signals[SIGNAL_DRAFT_CHANGED] =
        g_signal_new("draft-changed", CLAWT_TYPE_AGENT_DESIGNER,
                     G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
clawt_agent_designer_init(ClawtAgentDesigner *self)
{
    self->draft = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                        g_free);
    self->max_turns = 20;
}
