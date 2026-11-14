/*
 * clawt-model-fallback.c - The backends an agent continues on at a limit
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "config/clawt-model-fallback.h"

#include <string.h>

G_DEFINE_BOXED_TYPE(ClawtModelFallback, clawt_model_fallback,
                    clawt_model_fallback_copy, clawt_model_fallback_free)

ClawtModelFallback *
clawt_model_fallback_new(const gchar *provider,
                         const gchar *model,
                         const gchar *account)
{
    ClawtModelFallback *self = g_new0(ClawtModelFallback, 1);

    self->provider = g_strdup(provider);
    self->model = g_strdup(model);
    self->account = (account != NULL && *account != '\0')
                    ? g_strdup(account) : NULL;

    return self;
}

ClawtModelFallback *
clawt_model_fallback_copy(const ClawtModelFallback *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return clawt_model_fallback_new(self->provider, self->model,
                                    self->account);
}

void
clawt_model_fallback_free(ClawtModelFallback *self)
{
    if (self == NULL)
        return;

    g_free(self->provider);
    g_free(self->model);
    g_free(self->account);
    g_free(self);
}

gchar *
clawt_model_fallback_label(const ClawtModelFallback *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    if (self->account != NULL)
        return g_strdup_printf("%s %s as %s", self->provider,
                               self->model != NULL ? self->model : "",
                               self->account);

    return g_strdup_printf("%s %s", self->provider,
                           self->model != NULL ? self->model : "");
}

GPtrArray *
clawt_model_fallback_parse_list(const gchar  *text,
                                GError      **error)
{
    g_autoptr(GPtrArray) out = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_model_fallback_free);
    g_auto(GStrv) parts = NULL;
    guint i;

    if (text == NULL)
        return g_steal_pointer(&out);

    parts = g_strsplit(text, ",", -1);

    for (i = 0; parts[i] != NULL; i++) {
        gchar *entry = g_strstrip(parts[i]);
        g_autofree gchar *provider = NULL;
        g_autofree gchar *model = NULL;
        g_autofree gchar *account = NULL;
        const gchar *colon;
        const gchar *at;

        if (*entry == '\0')
            continue;

        colon = strchr(entry, ':');
        if (colon == NULL) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                        "fallback '%s' names no model: write it as "
                        "provider:model, or provider:model@account", entry);
            return NULL;
        }

        provider = g_strstrip(g_strndup(entry, (gsize)(colon - entry)));

        at = strrchr(colon + 1, '@');
        if (at != NULL) {
            model = g_strstrip(g_strndup(colon + 1, (gsize)(at - colon - 1)));
            account = g_strstrip(g_strdup(at + 1));
        } else {
            model = g_strstrip(g_strdup(colon + 1));
        }

        if (*provider == '\0' || *model == '\0') {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                        "fallback '%s' needs both a provider and a model",
                        entry);
            return NULL;
        }

        g_ptr_array_add(out, clawt_model_fallback_new(provider, model,
                                                      account));
    }

    return g_steal_pointer(&out);
}

gchar *
clawt_model_fallback_list_to_string(GPtrArray *fallbacks)
{
    GString *out = g_string_new(NULL);
    guint i;

    for (i = 0; fallbacks != NULL && i < fallbacks->len; i++) {
        const ClawtModelFallback *entry = g_ptr_array_index(fallbacks, i);

        if (out->len > 0)
            g_string_append(out, ", ");

        g_string_append_printf(out, "%s:%s",
                               entry->provider != NULL ? entry->provider : "",
                               entry->model != NULL ? entry->model : "");

        if (entry->account != NULL)
            g_string_append_printf(out, "@%s", entry->account);
    }

    return g_string_free(out, FALSE);
}

gboolean
clawt_model_fallback_account_fits(const gchar *account_provider,
                                  const gchar *provider)
{
    /*
     * As written, not normalised.  libreclaw folds "claude" into
     * claude-code and "grok" into grok-build, but the catalogue uses both
     * names for HTTP providers no agent runs on, so normalising would fit
     * an API key's entry to a subscription login.  accounts.provider is
     * documented in the canonical spelling, and a login spelt otherwise
     * is still saved -- the daemon's check is the one that decides.
     */
    if (account_provider == NULL || provider == NULL ||
        *account_provider == '\0' || *provider == '\0')
        return FALSE;

    return g_strcmp0(account_provider, provider) == 0;
}

const gchar *
clawt_failover_reason_phrase(const gchar *reason)
{
    if (g_strcmp0(reason, "usage-limit") == 0)
        return "hit its usage limit";
    if (g_strcmp0(reason, "rate-limit") == 0)
        return "is rate limited";
    if (g_strcmp0(reason, "signed-out") == 0)
        return "is signed out";
    if (g_strcmp0(reason, "unavailable") == 0)
        return "is unavailable";

    return "refused a turn";
}

gboolean
clawt_failover_is_active(JsonObject *failover)
{
    if (failover == NULL || !json_object_has_member(failover, "state"))
        return FALSE;

    return g_strcmp0(json_object_get_string_member(failover, "state"),
                     "fallback") == 0;
}

/*
 * "codex-cli gpt-5.5" out of a {provider, model} member.
 */
static gchar *
backend_label(JsonObject *failover, const gchar *member)
{
    JsonObject *backend;
    const gchar *provider;
    const gchar *model;

    if (failover == NULL || !json_object_has_member(failover, member))
        return g_strdup("?");

    backend = json_object_get_object_member(failover, member);
    if (backend == NULL)
        return g_strdup("?");

    provider = json_object_get_string_member_with_default(backend,
                                                          "provider", "?");
    model = json_object_get_string_member_with_default(backend, "model",
                                                       NULL);

    return model != NULL ? g_strdup_printf("%s %s", provider, model)
                         : g_strdup(provider);
}

/*
 * A Unix time as a local clock, with the day only when it is not today.
 */
static gchar *
clock_label(gint64 when)
{
    g_autoptr(GDateTime) local = NULL;
    g_autoptr(GDateTime) now = NULL;

    if (when <= 0)
        return NULL;

    local = g_date_time_new_from_unix_local(when);
    now = g_date_time_new_now_local();
    if (local == NULL || now == NULL)
        return NULL;

    if (g_date_time_get_year(local) == g_date_time_get_year(now) &&
        g_date_time_get_day_of_year(local) == g_date_time_get_day_of_year(now))
        return g_date_time_format(local, "%H:%M");

    /* %-d rather than %e: %e pads with a figure space g_strstrip misses. */
    return g_date_time_format(local, "%a %-d %b %H:%M");
}

gchar *
clawt_failover_badge(JsonObject *failover)
{
    g_autofree gchar *active = NULL;

    if (!clawt_failover_is_active(failover))
        return NULL;

    active = backend_label(failover, "active");

    return g_strdup_printf("On fallback: %s", active);
}

gchar *
clawt_failover_describe(const gchar *agent,
                        JsonObject  *failover)
{
    g_autofree gchar *primary = NULL;
    g_autofree gchar *active = NULL;
    g_autofree gchar *back = NULL;
    const gchar *who = (agent != NULL && *agent != '\0') ? agent : "the agent";
    GString *out;
    gint64 reset_at;
    gint64 retry_at;

    if (failover == NULL)
        return NULL;

    primary = backend_label(failover, "primary");

    if (!clawt_failover_is_active(failover))
        return g_strdup_printf("%s is back on %s", who, primary);

    active = backend_label(failover, "active");
    reset_at = json_object_get_int_member_with_default(failover, "reset_at", 0);
    retry_at = json_object_get_int_member_with_default(failover, "retry_at", 0);

    out = g_string_new(NULL);
    g_string_append_printf(out, "%s is running on fallback %s because %s %s",
                           who, active, primary,
                           clawt_failover_reason_phrase(
                               json_object_get_string_member_with_default(
                                   failover, "reason", NULL)));

    /*
     * A stated reset is a time the provider gave; a backoff is ours, and
     * is said as an attempt rather than a promise.
     */
    if (reset_at > 0 && (back = clock_label(reset_at)) != NULL)
        g_string_append_printf(out, ", back at %s", back);
    else if (retry_at > 0 && (back = clock_label(retry_at)) != NULL)
        g_string_append_printf(out, ", trying %s again at %s", primary, back);

    if (json_object_get_boolean_member_with_default(failover, "context_lost",
                                                    FALSE))
        g_string_append(out, " (a fresh conversation there: the context "
                             "did not come along)");

    return g_string_free(out, FALSE);
}
