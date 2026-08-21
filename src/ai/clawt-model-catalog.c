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

/*
 * The Claude Code CLI takes short aliases and resolves them itself, so
 * these are the aliases rather than dated model ids: an alias keeps
 * working when a new snapshot lands, and a pinned id quietly does not.
 */
static const ClawtModelInfo claude_code_models[] = {
    { "opus",   "Opus",   "the most capable; slowest and dearest" },
    { "sonnet", "Sonnet", "the usual choice" },
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

static const ClawtModelInfo grok_models[] = {
    { "grok-4", "Grok 4", NULL },
    { "grok-3", "Grok 3", NULL }
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

static const ClawtProviderInfo providers[] = {
    { "claude-code", "Claude Code",
      "the CLI, billed against your subscription",
      claude_code_models, G_N_ELEMENTS(claude_code_models), TRUE },

    { "claude-tmux", "Claude Code (tmux)",
      "the CLI in interactive mode; billed as ordinary subscription use "
      "rather than against the Agent SDK credit pool",
      claude_code_models, G_N_ELEMENTS(claude_code_models), TRUE },

    { "claude", "Claude API",
      "the HTTP API; needs an API key and is billed per token",
      claude_api_models, G_N_ELEMENTS(claude_api_models), TRUE },

    { "openai", "OpenAI",
      "needs an API key",
      openai_models, G_N_ELEMENTS(openai_models), TRUE },

    { "gemini", "Google Gemini",
      "needs an API key",
      gemini_models, G_N_ELEMENTS(gemini_models), TRUE },

    { "grok", "xAI Grok",
      "needs an API key",
      grok_models, G_N_ELEMENTS(grok_models), TRUE },

    { "opencode", "OpenCode",
      "the OpenCode CLI wrapper",
      NULL, 0, TRUE },

    { "ollama", "Ollama",
      "local models; whatever you have pulled",
      ollama_models, G_N_ELEMENTS(ollama_models), TRUE }
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
