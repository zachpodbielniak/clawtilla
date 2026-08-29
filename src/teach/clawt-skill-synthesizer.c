/*
 * clawt-skill-synthesizer.c - Turning a recording into a skill draft
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "teach/clawt-skill-synthesizer.h"

#include "skill/clawt-skill-scan.h"

#include <string.h>

struct _ClawtSkillSynthesizer {
    GObject parent_instance;

    ClawtSkillLibrary *library;
    ClawtConfig       *config;
    AiProvider        *provider;
    AiToolExecutor    *executor;

    GHashTable        *draft;     /* name, description, body */
    gchar             *transcript;
    gchar             *origin;
    guint              max_turns;
};

G_DEFINE_FINAL_TYPE(ClawtSkillSynthesizer, clawt_skill_synthesizer,
                    G_TYPE_OBJECT)

/*
 * How much of a trace the model is shown.
 *
 * A trace can hold twenty thousand steps, and handing all of them over
 * would be a prompt nobody can afford and a procedure written from the
 * noise. The cap is stated in the rendering rather than applied
 * silently, so a model that has been given a prefix knows it has one.
 */
#define SYNTHESIS_MAX_STEPS (400)

static void register_tools(ClawtSkillSynthesizer *self);

ClawtSkillSynthesizer *
clawt_skill_synthesizer_new(ClawtSkillLibrary *library, ClawtConfig *config)
{
    ClawtSkillSynthesizer *self;

    g_return_val_if_fail(CLAWT_IS_SKILL_LIBRARY(library), NULL);

    self = g_object_new(CLAWT_TYPE_SKILL_SYNTHESIZER, NULL);
    self->library = g_object_ref(library);

    if (config != NULL)
        self->config = g_object_ref(config);

    /*
     * new_empty(), emphatically not new().
     *
     * ai_tool_executor_new() pre-registers bash, read, write and edit
     * and unregister() cannot take them back.  A model asked to write
     * markdown from a list of steps does not need a shell, and this one
     * has just been handed a transcript of somebody's keyboard.
     */
    self->executor = ai_tool_executor_new_empty();
    register_tools(self);

    return self;
}

void
clawt_skill_synthesizer_set_provider(ClawtSkillSynthesizer *self,
                                     AiProvider            *provider)
{
    g_return_if_fail(CLAWT_IS_SKILL_SYNTHESIZER(self));

    g_clear_object(&self->provider);

    if (provider != NULL)
        self->provider = g_object_ref(provider);
}

gboolean
clawt_skill_synthesizer_set_provider_by_name(ClawtSkillSynthesizer  *self,
                                             const gchar            *provider_name,
                                             const gchar            *model,
                                             GError                **error)
{
    g_autoptr(AiConfig) ai_config = NULL;
    GObject *provider;

    g_return_val_if_fail(CLAWT_IS_SKILL_SYNTHESIZER(self), FALSE);

    if (provider_name == NULL || *provider_name == '\0')
        provider_name = "claude-code";

    ai_config = ai_config_new();

    if (model != NULL && *model != '\0')
        ai_config_set_default_model(ai_config, model);

    provider = ai_provider_factory_new_from_string(provider_name, ai_config,
                                                   error);

    if (provider == NULL) {
        g_prefix_error(error, "AI provider '%s': ", provider_name);
        return FALSE;
    }

    clawt_skill_synthesizer_set_provider(self, AI_PROVIDER(provider));
    g_object_unref(provider);

    return TRUE;
}

gboolean
clawt_skill_synthesizer_use_configured_provider(ClawtSkillSynthesizer  *self,
                                                GError                **error)
{
    g_return_val_if_fail(CLAWT_IS_SKILL_SYNTHESIZER(self), FALSE);

    if (self->config == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                            "no configuration to read ai_assist from");
        return FALSE;
    }

    if (!clawt_config_get_boolean(self->config, "ai_assist.enabled")) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                            "AI assistance is turned off; set "
                            "ai_assist.enabled: true");
        return FALSE;
    }

    if (!clawt_skill_synthesizer_set_provider_by_name(
            self, clawt_config_get_string(self->config, "ai_assist.provider"),
            clawt_config_get_string(self->config, "ai_assist.model"), error))
        return FALSE;

    self->max_turns = (guint)clawt_config_get_int(self->config,
                                                  "ai_assist.max_turns");

    return TRUE;
}

void
clawt_skill_synthesizer_set_max_turns(ClawtSkillSynthesizer *self,
                                      guint                  max_turns)
{
    g_return_if_fail(CLAWT_IS_SKILL_SYNTHESIZER(self));

    self->max_turns = max_turns;
}

GHashTable *
clawt_skill_synthesizer_get_draft(ClawtSkillSynthesizer *self)
{
    g_return_val_if_fail(CLAWT_IS_SKILL_SYNTHESIZER(self), NULL);

    return self->draft;
}

const gchar *
clawt_skill_synthesizer_get_transcript(ClawtSkillSynthesizer *self)
{
    g_return_val_if_fail(CLAWT_IS_SKILL_SYNTHESIZER(self), NULL);

    return self->transcript;
}

/* ── The draft, and the one place it becomes a SKILL.md ──────────── */

/*
 * The draft rendered as the file it would be.
 *
 * One function, used by the preview, by the commit tool's validation
 * and by the tests -- so what is validated, what is shown and what is
 * written are the same bytes. Three renderings would agree until one of
 * them gained a trailing newline.
 */
static gchar *
render_draft(ClawtSkillSynthesizer *self)
{
    g_autoptr(ClawtSkill) skill = NULL;
    const gchar *name = g_hash_table_lookup(self->draft, "name");
    const gchar *description = g_hash_table_lookup(self->draft,
                                                   "description");
    const gchar *body = g_hash_table_lookup(self->draft, "body");

    skill = (name != NULL) ? clawt_skill_new(name) : NULL;

    if (skill == NULL)
        return g_strdup("");

    clawt_skill_set_description(skill, (description != NULL) ? description
                                                             : "");
    clawt_skill_set_body(skill, (body != NULL) ? body : "");

    return clawt_skill_render(skill);
}

gchar *
clawt_skill_synthesizer_preview(ClawtSkillSynthesizer *self)
{
    g_return_val_if_fail(CLAWT_IS_SKILL_SYNTHESIZER(self), NULL);

    return render_draft(self);
}

/*
 * Everything the commit path checks, run before the model is told it is
 * done.
 *
 * The parse is the whole of clawtilla's skill validation -- front
 * matter on line one, the name rules, the description bound, the scan --
 * so running it here means a model that wrote something unusable is
 * told what was wrong while it still has a turn left, rather than the
 * person finding out at commit and having to re-run the whole
 * conversation.
 */
static gboolean
validate_draft(ClawtSkillSynthesizer *self, GError **error)
{
    g_autofree gchar *rendered = NULL;
    g_autoptr(ClawtSkill) parsed = NULL;
    const gchar *name = g_hash_table_lookup(self->draft, "name");

    if (name == NULL || g_hash_table_lookup(self->draft, "body") == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "the skill needs a name, a description and a "
                            "body; call set_name, set_description and "
                            "write_body first");
        return FALSE;
    }

    rendered = render_draft(self);
    parsed = clawt_skill_parse(rendered, -1, name, error);

    return parsed != NULL;
}

/* ── The tools ───────────────────────────────────────────────────── */

static gchar *
tool_set_name(AiToolUse *use, GCancellable *cancellable, GError **error,
              gpointer user_data)
{
    ClawtSkillSynthesizer *self = user_data;
    const gchar *name = ai_tool_use_get_input_string(use, "name");

    (void)cancellable;

    if (name == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "name is required");
        return NULL;
    }

    /*
     * Refused with the rule stated rather than with a bare "invalid".
     * A model told only no tries the same thing again, and this name is
     * also the traversal gate -- it becomes a directory.
     */
    if (!clawt_skill_name_is_valid(name)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "'%s' is not a skill name: lowercase letters, digits "
                    "and single hyphens, no dots and no slashes, at most "
                    "%d characters", name, CLAWT_SKILL_MAX_NAME);
        return NULL;
    }

    g_hash_table_insert(self->draft, g_strdup("name"), g_strdup(name));

    return g_strdup_printf("The skill will be called %s.", name);
}

static gchar *
tool_set_description(AiToolUse *use, GCancellable *cancellable,
                     GError **error, gpointer user_data)
{
    ClawtSkillSynthesizer *self = user_data;
    const gchar *text = ai_tool_use_get_input_string(use, "description");

    (void)cancellable;

    if (text == NULL || *text == '\0') {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "description is required");
        return NULL;
    }

    if (strlen(text) > CLAWT_SKILL_MAX_DESCRIPTION) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "that description is %" G_GSIZE_FORMAT " characters and "
                    "the limit is %d: it is in every agent's context on "
                    "every turn, so it has to be one sentence",
                    strlen(text), CLAWT_SKILL_MAX_DESCRIPTION);
        return NULL;
    }

    g_hash_table_insert(self->draft, g_strdup("description"),
                        g_strdup(text));

    return g_strdup("Description set.");
}

static gchar *
tool_write_body(AiToolUse *use, GCancellable *cancellable, GError **error,
                gpointer user_data)
{
    ClawtSkillSynthesizer *self = user_data;
    const gchar *body = ai_tool_use_get_input_string(use, "body");

    (void)cancellable;

    if (body == NULL || *body == '\0') {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "body is required");
        return NULL;
    }

    g_hash_table_insert(self->draft, g_strdup("body"), g_strdup(body));

    return g_strdup_printf("Body written, %" G_GSIZE_FORMAT " characters.",
                           strlen(body));
}

static gchar *
tool_preview(AiToolUse *use, GCancellable *cancellable, GError **error,
             gpointer user_data)
{
    ClawtSkillSynthesizer *self = user_data;

    (void)use;
    (void)cancellable;
    (void)error;

    return render_draft(self);
}

static gchar *
tool_commit(AiToolUse *use, GCancellable *cancellable, GError **error,
            gpointer user_data)
{
    ClawtSkillSynthesizer *self = user_data;

    (void)use;
    (void)cancellable;

    /*
     * Validated here, and the failure goes back to the model as a tool
     * error rather than being kept for the person.  A draft that will
     * not parse is one the model can still fix; discovering it at
     * commit means running the whole conversation again.
     */
    if (!validate_draft(self, error))
        return NULL;

    /*
     * Validated, not written.  The draft becomes a skill only when a
     * person says so -- and it lands disabled even then, because the
     * trace it was written from is untrusted the moment it contains
     * anything somebody typed.
     */
    return g_strdup("Draft ready. It will land disabled for review.");
}

static void
register_tool(ClawtSkillSynthesizer *self, const gchar *name,
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
register_tools(ClawtSkillSynthesizer *self)
{
    static const ClawtParamInfo name_params[] = {
        { "name", "string", "Lowercase letters, digits and single hyphens. "
                            "It is also the directory name.", TRUE }
    };

    static const ClawtParamInfo description_params[] = {
        { "description", "string", "One sentence beginning \"Use this "
                                   "when ...\". It is the only part an "
                                   "agent reads before opening the skill.",
          TRUE }
    };

    static const ClawtParamInfo body_params[] = {
        { "body", "string", "The procedure, in markdown, without front "
                            "matter.", TRUE }
    };

    static const ClawtParamInfo no_params[] = {
        { NULL, NULL, NULL, FALSE }
    };

    register_tool(self, "set_name", "Name the skill. Call this first.",
                  tool_set_name, name_params, G_N_ELEMENTS(name_params));

    register_tool(self, "set_description",
                  "Say when an agent should reach for this skill.",
                  tool_set_description, description_params,
                  G_N_ELEMENTS(description_params));

    register_tool(self, "write_body",
                  "Write the procedure itself, in markdown.",
                  tool_write_body, body_params, G_N_ELEMENTS(body_params));

    register_tool(self, "preview",
                  "Show the SKILL.md as it stands.",
                  tool_preview, no_params, 0);

    register_tool(self, "commit",
                  "Finish. Call this when the skill is complete.",
                  tool_commit, no_params, 0);
}

/* ── Running it ──────────────────────────────────────────────────── */

static gchar *
build_system_prompt(ClawtTeachTrace *trace)
{
    g_autoptr(GString) caveats = g_string_new(NULL);
    GPtrArray *list = clawt_teach_trace_get_caveats(trace);
    guint i;

    for (i = 0; list != NULL && i < list->len; i++)
        g_string_append_printf(caveats, "- %s\n",
                               (const gchar *)g_ptr_array_index(list, i));

    return g_strdup_printf(
        "You are writing one reusable skill from a recording of a task "
        "being done.\n"
        "\n"
        "A skill is a SKILL.md: YAML front matter with a name and a\n"
        "description, then markdown saying how to do the thing. Use the\n"
        "tools to fill in the three parts and then call commit.\n"
        "\n"
        "Write the *procedure*, not a narrative of the recording. The\n"
        "recording is one run of the task; the skill is how to do it\n"
        "again, so generalise the paths, names and values that were\n"
        "specific to that run and say what they stand for.\n"
        "\n"
        "The description is the only part an agent reads before deciding\n"
        "to open the skill, so write it as \"Use this when ...\" rather\n"
        "than as a title.\n"
        "\n"
        "Two things you must not do, and they are the reason this draft\n"
        "is reviewed by a person before it is enabled:\n"
        "- Do not copy any password, token, key or other credential out\n"
        "of the recording, in any form. If a step looks like somebody\n"
        "typing a secret, write what it was for and not what it was.\n"
        "- Do not invent steps the recording does not show. A gap in a\n"
        "recording is a gap; say so rather than filling it in.\n"
        "\n"
        "How this recording was captured, which bounds what you can\n"
        "rely on:\n"
        "%s"
        "\n"
        "Write markdown, not org-mode. Keep it short enough to read.",
        (caveats->len > 0) ? caveats->str
                           : "- Nothing was recorded about how it was "
                             "captured.\n");
}

GHashTable *
clawt_skill_synthesizer_synthesize(ClawtSkillSynthesizer  *self,
                                   ClawtTeachTrace        *trace,
                                   GCancellable           *cancellable,
                                   GError                **error)
{
    g_autoptr(AiMessage) message = NULL;
    g_autofree gchar *reply = NULL;
    g_autofree gchar *system_prompt = NULL;
    g_autofree gchar *rendered_trace = NULL;
    GList *messages = NULL;

    g_return_val_if_fail(CLAWT_IS_SKILL_SYNTHESIZER(self), NULL);
    g_return_val_if_fail(trace != NULL, NULL);

    if (self->provider == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_AI,
                            "no AI provider is configured; set "
                            "ai_assist.provider in clawtilla.yaml");
        return NULL;
    }

    /*
     * Refused before the call, exactly as the designer refuses.
     *
     * ai-glib's CLI clients drop the tool list, so a model reached that
     * way is given a system prompt describing tools it never receives
     * and answers in prose -- which surfaces as "it never called
     * set_name" after a full round trip and reads as the model being
     * uncooperative rather than the provider being unable.
     */
    if (AI_IS_CLI_CLIENT(self->provider)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                    "%s runs through a command-line tool, which ai-glib "
                    "cannot pass tool definitions to, and writing a skill "
                    "works entirely through tools. Set ai_assist.provider "
                    "to an API provider -- claude, openai, gemini or grok "
                    "-- and give it a key.",
                    G_OBJECT_TYPE_NAME(self->provider));
        return NULL;
    }

    g_free(self->origin);
    self->origin = g_strdup_printf("teach:%s",
                                   clawt_teach_trace_get_id(trace));

    system_prompt = build_system_prompt(trace);
    rendered_trace = clawt_teach_trace_render(trace, SYNTHESIS_MAX_STEPS);

    message = ai_message_new_user(rendered_trace);
    messages = g_list_append(NULL, message);

    reply = ai_tool_executor_run(self->executor, self->provider, messages,
                                 system_prompt, self->max_turns, cancellable,
                                 error);

    g_list_free(messages);

    if (reply == NULL)
        return NULL;

    g_free(self->transcript);
    self->transcript = g_steal_pointer(&reply);

    if (g_hash_table_lookup(self->draft, "name") == NULL) {
        /*
         * What it said instead is included.  "It never called set_name"
         * on its own is unactionable: the reply usually says why -- a
         * refusal, a question, or a provider that never offered the
         * tools -- and without it the only way to find out is to make
         * the call again by hand.
         */
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_AI,
                    "the model did not write a skill: it never called "
                    "set_name. It said: %s",
                    (self->transcript != NULL && *self->transcript != '\0')
                    ? self->transcript : "(nothing)");
        return NULL;
    }

    return self->draft;
}

ClawtSkill *
clawt_skill_synthesizer_commit(ClawtSkillSynthesizer *self, GError **error)
{
    const gchar *name;

    g_return_val_if_fail(CLAWT_IS_SKILL_SYNTHESIZER(self), NULL);

    name = g_hash_table_lookup(self->draft, "name");

    if (name == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "there is nothing to commit");
        return NULL;
    }

    /*
     * Validated again here rather than trusting the tool's check.
     *
     * The draft is reachable between the two -- a person edits the name
     * in a client, or a second synthesis overwrites the body -- and the
     * rule this tree keeps re-learning is that a check at one call site
     * is a check about that call site.
     */
    if (!validate_draft(self, error))
        return NULL;

    return clawt_skill_library_create_taught(
        self->library, name,
        g_hash_table_lookup(self->draft, "description"),
        g_hash_table_lookup(self->draft, "body"),
        self->origin, error);
}

static void
clawt_skill_synthesizer_dispose(GObject *object)
{
    ClawtSkillSynthesizer *self = CLAWT_SKILL_SYNTHESIZER(object);

    g_clear_object(&self->library);
    g_clear_object(&self->config);
    g_clear_object(&self->provider);
    g_clear_object(&self->executor);

    G_OBJECT_CLASS(clawt_skill_synthesizer_parent_class)->dispose(object);
}

static void
clawt_skill_synthesizer_finalize(GObject *object)
{
    ClawtSkillSynthesizer *self = CLAWT_SKILL_SYNTHESIZER(object);

    g_clear_pointer(&self->draft, g_hash_table_unref);
    g_free(self->transcript);
    g_free(self->origin);

    G_OBJECT_CLASS(clawt_skill_synthesizer_parent_class)->finalize(object);
}

static void
clawt_skill_synthesizer_class_init(ClawtSkillSynthesizerClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = clawt_skill_synthesizer_dispose;
    G_OBJECT_CLASS(klass)->finalize = clawt_skill_synthesizer_finalize;
}

static void
clawt_skill_synthesizer_init(ClawtSkillSynthesizer *self)
{
    self->draft = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                        g_free);
    self->max_turns = 12;
}
