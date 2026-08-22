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

G_END_DECLS
