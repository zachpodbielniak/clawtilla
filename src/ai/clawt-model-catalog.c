/*
 * clawt-model-catalog.c - Which providers exist, and what they run
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "ai/clawt-model-catalog.h"

#include <ai-glib.h>

/*
 * The Claude Code CLI takes short aliases and resolves them itself, so
 * these are the aliases rather than dated model ids: an alias keeps
 * working when a new snapshot lands, and a pinned id quietly does not.
 */
static const ClawtModelInfo claude_code_models[] = {
    { "opus",   "Opus",   "the most capable; slowest and dearest" },
    { "sonnet", "Sonnet", "the usual choice" },
    { "fable",  "Fable",  "for writing rather than code" },
    { "haiku",  "Haiku",  "fast and cheap; good for narrow work" }
};

static const ClawtModelInfo claude_api_models[] = {
    { "claude-opus-5",             "Opus 5",   NULL },
    { "claude-sonnet-5",           "Sonnet 5", NULL },
    { "claude-fable-5",            "Fable 5",  NULL },
    { "claude-haiku-4-5-20251001", "Haiku 4.5", "fast and cheap" }
};

static const ClawtModelInfo openai_models[] = {
    { "gpt-4o",      "GPT-4o",      NULL },
    { "gpt-4o-mini", "GPT-4o mini", "fast and cheap" },
    { "o3",          "o3",          "reasoning" },
    { "o4-mini",     "o4-mini",     "reasoning, cheaper" }
};

static const ClawtModelInfo gemini_models[] = {
    { "gemini-2.5-pro",   "Gemini 2.5 Pro",   NULL },
    { "gemini-2.5-flash", "Gemini 2.5 Flash", "fast and cheap" }
};

/*
 * A fallback for when there is no key to ask with.
 *
 * xAI ships new models faster than a hardcoded table can track -- this
 * one still said grok-3 and grok-4 well after 4.5 and 4.6 had landed --
 * so `model.list refresh: true` asks the provider and these are only
 * what is offered when that cannot be done.
 */
static const ClawtModelInfo grok_models[] = {
    { "grok-4.6", "Grok 4.6", NULL },
    { "grok-4.5", "Grok 4.5", NULL },
    { "grok-4",   "Grok 4",   "previous generation" }
};

/*
 * The `grok` CLI, which is a different thing from the xAI HTTP API
 * above and takes a different, shorter list.  Not asked for over the
 * network: the CLI authenticates itself and there is no key here to
 * enumerate models with.
 */
static const ClawtModelInfo grok_build_models[] = {
    { "grok-4.6", "Grok 4.6", "the CLI's default" },
    { "grok-4.5", "Grok 4.5", "previous release" }
};

/*
 * Ollama runs whatever has been pulled locally, so these are only the
 * common ones.  open_ended is TRUE so a client offers a way to type a
 * name that is not here.
 */
static const ClawtModelInfo ollama_models[] = {
    { "llama3.3",   "Llama 3.3",   NULL },
    { "qwen2.5",    "Qwen 2.5",    NULL },
    { "mistral",    "Mistral",     NULL },
    { "deepseek-r1", "DeepSeek-R1", "reasoning" }
};

/*
 * Ordered so the ones an agent can actually run on come first.
 *
 * The `agent` column is libreclaw's list, not a preference: its
 * provider table is command-line only, and lc_provider_type_normalize()
 * silently rewrites anything it does not know to claude-code.  An agent
 * configured for "openai" was therefore running Claude Code with
 * "gpt-4o" in the model field -- so those providers are offered for
 * designing an agent, where ai-glib drives them over HTTP directly, and
 * not for being one.
 */
static const ClawtProviderInfo providers[] = {
    /* ── can back an agent: libreclaw's four CLI backends ────────── */
    { "claude-code", "Claude Code",
      "the CLI, billed against your subscription",
      claude_code_models, G_N_ELEMENTS(claude_code_models),
      TRUE, TRUE, FALSE },

    { "claude-tmux", "Claude Code (tmux)",
      "the CLI in interactive mode; billed as ordinary subscription use "
      "rather than against the Agent SDK credit pool",
      claude_code_models, G_N_ELEMENTS(claude_code_models),
      TRUE, TRUE, FALSE },

    { "grok-build", "Grok CLI",
      "xAI's grok CLI in headless mode; it edits files and runs commands, "
      "and authenticates itself rather than taking a key from here",
      grok_build_models, G_N_ELEMENTS(grok_build_models),
      TRUE, TRUE, FALSE },

    { "opencode", "OpenCode",
      "the OpenCode CLI; routes to xAI, Google, OpenAI and others itself",
      NULL, 0, TRUE, TRUE, FALSE },

    /* ── can design an agent: ai-glib's HTTP providers ───────────── */
    { "claude", "Claude API",
      "the HTTP API; needs an API key and is billed per token",
      claude_api_models, G_N_ELEMENTS(claude_api_models),
      TRUE, FALSE, TRUE },

    { "openai", "OpenAI",
      "needs an API key",
      openai_models, G_N_ELEMENTS(openai_models), TRUE, FALSE, TRUE },

    { "gemini", "Google Gemini",
      "needs an API key",
      gemini_models, G_N_ELEMENTS(gemini_models), TRUE, FALSE, TRUE },

    { "grok", "xAI Grok",
      "the HTTP API; needs an API key. For an agent, pick Grok CLI",
      grok_models, G_N_ELEMENTS(grok_models), TRUE, FALSE, TRUE },

    { "ollama", "Ollama",
      "local models; whatever you have pulled",
      ollama_models, G_N_ELEMENTS(ollama_models), TRUE, FALSE, TRUE }
};

const ClawtProviderInfo *
clawt_model_catalog_get(gsize *n_providers)
{
    g_return_val_if_fail(n_providers != NULL, NULL);

    *n_providers = G_N_ELEMENTS(providers);

    return providers;
}

const ClawtProviderInfo *
clawt_model_catalog_find_provider(const gchar *provider_id)
{
    gsize i;

    if (provider_id == NULL)
        return NULL;

    for (i = 0; i < G_N_ELEMENTS(providers); i++) {
        if (g_strcmp0(providers[i].id, provider_id) == 0)
            return &providers[i];
    }

    return NULL;
}

const gchar *
clawt_model_catalog_default_provider(void)
{
    return providers[0].id;
}

/* ── Asking the provider ─────────────────────────────────────────── */

typedef struct {
    GList    *models;
    GError   *error;
    gboolean  done;
} FetchResult;

static void
on_models_listed(GObject *source, GAsyncResult *result, gpointer user_data)
{
    FetchResult *fetch = user_data;

    fetch->models = ai_provider_list_models_finish(AI_PROVIDER(source),
                                                    result, &fetch->error);
    fetch->done = TRUE;
}

static gboolean
on_fetch_timeout(gpointer user_data)
{
    FetchResult *fetch = user_data;

    if (!fetch->done) {
        fetch->done = TRUE;
        g_set_error_literal(&fetch->error, CLAWT_ERROR, CLAWT_ERROR_AI,
                            "the provider did not answer in time");
    }

    return G_SOURCE_REMOVE;
}

GStrv
clawt_model_catalog_fetch_models(const gchar   *provider_id,
                                 GMainContext  *context,
                                 guint          timeout_seconds,
                                 GError       **error)
{
    g_autoptr(AiConfig) ai_config = NULL;
    g_autoptr(GPtrArray) names = NULL;
    GObject *provider;
    FetchResult fetch = { 0 };
    GSource *timeout;
    GList *l;

    g_return_val_if_fail(provider_id != NULL, NULL);

    ai_config = ai_config_new();
    provider = ai_provider_factory_new_from_string(provider_id, ai_config,
                                                    error);

    if (provider == NULL) {
        g_prefix_error(error, "%s: ", provider_id);
        return NULL;
    }

    ai_provider_list_models_async(AI_PROVIDER(provider), NULL,
                                   on_models_listed, &fetch);

    /*
     * A bound, because a provider that never answers would otherwise
     * hang the daemon's IPC handler and every client waiting on it.
     * Attached to the caller's context, not the default one -- an
     * embedded daemon has no default context running.
     */
    timeout = g_timeout_source_new_seconds(
        timeout_seconds > 0 ? timeout_seconds : 15);
    g_source_set_callback(timeout, on_fetch_timeout, &fetch, NULL);
    g_source_attach(timeout, context);

    while (!fetch.done)
        g_main_context_iteration(context, TRUE);

    g_source_destroy(timeout);
    g_source_unref(timeout);
    g_object_unref(provider);

    if (fetch.error != NULL) {
        g_propagate_prefixed_error(error, fetch.error, "%s: ", provider_id);
        return NULL;
    }

    names = g_ptr_array_new_with_free_func(g_free);

    for (l = fetch.models; l != NULL; l = l->next) {
        if (l->data != NULL)
            g_ptr_array_add(names, g_strdup(l->data));
    }

    g_list_free_full(fetch.models, g_free);
    g_ptr_array_add(names, NULL);

    return (GStrv)g_ptr_array_free(g_steal_pointer(&names), FALSE);
}

/* ── Asking without waiting ──────────────────────────────────────── */

typedef struct {
    gchar                *provider_id;
    ClawtModelsReadyFunc  ready;
    gpointer              user_data;
    GObject              *provider;
} AsyncFetch;

static void
async_fetch_free(AsyncFetch *fetch)
{
    g_clear_object(&fetch->provider);
    g_free(fetch->provider_id);
    g_free(fetch);
}

static void
on_models_listed_async(GObject *source, GAsyncResult *result,
                       gpointer user_data)
{
    AsyncFetch *fetch = user_data;
    g_autoptr(GError) error = NULL;
    g_auto(GStrv) names = NULL;
    GList *models;

    models = ai_provider_list_models_finish(AI_PROVIDER(source), result,
                                             &error);

    if (models != NULL) {
        g_autoptr(GPtrArray) array =
            g_ptr_array_new_with_free_func(g_free);
        GList *l;

        for (l = models; l != NULL; l = l->next) {
            if (l->data != NULL)
                g_ptr_array_add(array, g_strdup(l->data));
        }

        g_list_free_full(models, g_free);
        g_ptr_array_add(array, NULL);
        names = (GStrv)g_ptr_array_free(g_steal_pointer(&array), FALSE);
    } else if (error != NULL) {
        /*
         * Debug, not a warning.  A provider with no key is the ordinary
         * case -- most people have one or two -- and a warning per
         * provider per refresh would be noise in every log.
         */
        g_debug("model catalogue: %s: %s", fetch->provider_id,
                error->message);
    }

    if (fetch->ready != NULL)
        fetch->ready(fetch->provider_id, names, fetch->user_data);

    async_fetch_free(fetch);
}

void
clawt_model_catalog_fetch_models_async(const gchar          *provider_id,
                                       ClawtModelsReadyFunc  ready,
                                       gpointer              user_data)
{
    g_autoptr(AiConfig) ai_config = NULL;
    g_autoptr(GError) error = NULL;
    AsyncFetch *fetch;
    GObject *provider;

    g_return_if_fail(provider_id != NULL);

    ai_config = ai_config_new();
    provider = ai_provider_factory_new_from_string(provider_id, ai_config,
                                                    &error);

    if (provider == NULL) {
        g_debug("model catalogue: %s: %s", provider_id,
                error != NULL ? error->message : "unavailable");

        if (ready != NULL)
            ready(provider_id, NULL, user_data);

        return;
    }

    fetch = g_new0(AsyncFetch, 1);
    fetch->provider_id = g_strdup(provider_id);
    fetch->ready = ready;
    fetch->user_data = user_data;
    fetch->provider = provider;

    ai_provider_list_models_async(AI_PROVIDER(provider), NULL,
                                   on_models_listed_async, fetch);
}
