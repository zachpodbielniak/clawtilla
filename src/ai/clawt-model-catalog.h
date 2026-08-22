/*
 * clawt-model-catalog.h - Which providers exist, and what they run
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-types.h"

G_BEGIN_DECLS

/**
 * ClawtModelInfo:
 * @id: what goes in `model.model`
 * @label: how to show it to a person
 * @note: (nullable): anything worth knowing before picking it
 *
 * One model.
 */
typedef struct {
    const gchar *id;
    const gchar *label;
    const gchar *note;
} ClawtModelInfo;

/**
 * ClawtProviderInfo:
 * @id: what goes in `model.provider`
 * @label: how to show it to a person
 * @note: (nullable): how this provider is reached or billed
 * @models: (array length=n_models): the models it runs
 * @n_models: how many
 * @open_ended: %TRUE when the list is a starting point rather than the
 *   whole truth -- Ollama runs whatever you have pulled, so a client must
 *   let a person type a name that is not listed
 * @tools: %TRUE when this provider can be given tool definitions
 *
 * One provider and its models.
 *
 * @tools is what separates an HTTP API from a wrapped command-line tool.
 * ai-glib's CLI clients drop the tool list rather than passing it on, so
 * anything built on tool calls -- the agent designer -- simply cannot
 * use them. Agents themselves run fine either way; this is only about
 * whether a provider can be handed a set of tools and asked to use them.
 */
typedef struct {
    const gchar          *id;
    const gchar          *label;
    const gchar          *note;
    const ClawtModelInfo *models;
    gsize                 n_models;
    gboolean              open_ended;
    gboolean              tools;
} ClawtProviderInfo;

/**
 * clawt_model_catalog_get:
 * @n_providers: (out): how many providers there are
 *
 * The providers clawtilla knows about, in the order to show them.
 *
 * A curated list rather than something discovered at runtime: most
 * providers have no endpoint that enumerates their models, and one that
 * asked every provider on every dialog open would be slow and would fail
 * whenever a key was missing.  It is therefore a list that goes stale --
 * which is why every provider also accepts a model name typed by hand,
 * and why nothing validates `model.model` against this table.
 *
 * Returns: (array length=n_providers) (transfer none): the providers
 */
const ClawtProviderInfo *clawt_model_catalog_get(gsize *n_providers);

/**
 * clawt_model_catalog_find_provider:
 * @provider_id: a `model.provider` value
 *
 * Returns: (transfer none) (nullable): what is known about it, or %NULL
 */
const ClawtProviderInfo *clawt_model_catalog_find_provider(
    const gchar *provider_id);

/**
 * clawt_model_catalog_default_provider:
 *
 * Returns: (transfer none): the provider to offer first
 */
const gchar *clawt_model_catalog_default_provider(void);

/**
 * clawt_model_catalog_fetch_models:
 * @provider_id: a provider id from the catalogue
 * @context: (nullable): the #GMainContext to pump while waiting
 * @timeout_seconds: how long to wait before giving up
 * @error: (out) (optional): return location for a #GError
 *
 * Asks the provider which models it actually runs, right now.
 *
 * The hardcoded catalogue goes stale -- it listed grok-3 and grok-4 well
 * after 4.5 and 4.6 had shipped -- so anywhere a list is shown to a
 * person should prefer this and fall back to the catalogue when there is
 * no key to ask with, or no network.
 *
 * Synchronous, because every caller is an IPC handler that is itself
 * synchronous. The context is pumped while waiting rather than blocked,
 * so the daemon keeps answering.
 *
 * Returns: (transfer full) (nullable) (array zero-terminated=1): the
 *   model ids, or %NULL on failure
 */
GStrv clawt_model_catalog_fetch_models(const gchar   *provider_id,
                                       GMainContext  *context,
                                       guint          timeout_seconds,
                                       GError       **error);

/**
 * ClawtModelsReadyFunc:
 * @provider_id: which provider answered
 * @models: (nullable) (array zero-terminated=1): what it runs, or %NULL
 * @user_data: as passed in
 *
 * Called when a provider has answered, or failed to.
 */
typedef void (*ClawtModelsReadyFunc)(const gchar *provider_id,
                                     GStrv        models,
                                     gpointer     user_data);

/**
 * clawt_model_catalog_fetch_models_async:
 * @provider_id: a provider id from the catalogue
 * @ready: called when the provider answers
 * @user_data: passed to @ready
 *
 * Asks a provider what it runs, without waiting.
 *
 * The synchronous version pumps the caller's context, which is fine for
 * a CLI and wrong for a daemon: a client asking for the model list got
 * a request that took as long as the slowest provider, and both the
 * new-agent dialog and the agent inspector ask on every build. The
 * result was a UI that appeared to hang on a button press.
 */
void clawt_model_catalog_fetch_models_async(const gchar          *provider_id,
                                            ClawtModelsReadyFunc  ready,
                                            gpointer              user_data);

G_END_DECLS
