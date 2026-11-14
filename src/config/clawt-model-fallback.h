/*
 * clawt-model-fallback.h - The backends an agent continues on at a limit
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * `agents.model.fallbacks` is an ordered list of {provider, model,
 * account}.  libreclaw does the failing over -- it is the one that sees
 * the provider's refusal -- so what lives here is the shape of the list,
 * its one text spelling (what `agent set` takes and both clients send),
 * and the words for what an agent reports when it is running on one.
 * A rule both clients apply belongs in the library, where it is testable
 * without a window or a browser.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * ClawtModelFallback:
 * @provider: the agent CLI, as `model.provider` spells it
 * @model: the model that CLI is asked for
 * @account: (nullable): the entry of `accounts:` it logs in as, or
 *   %NULL for the CLI's default login
 *
 * One entry of `agents.model.fallbacks`.
 */
typedef struct {
    gchar *provider;
    gchar *model;
    gchar *account;
} ClawtModelFallback;

#define CLAWT_TYPE_MODEL_FALLBACK (clawt_model_fallback_get_type())

GType clawt_model_fallback_get_type(void) G_GNUC_CONST;

/**
 * clawt_model_fallback_new:
 * @provider: the agent CLI
 * @model: the model
 * @account: (nullable): an account id, or %NULL (an empty string is %NULL)
 *
 * Returns: (transfer full): a new #ClawtModelFallback
 */
ClawtModelFallback *clawt_model_fallback_new(const gchar *provider,
                                             const gchar *model,
                                             const gchar *account);

/**
 * clawt_model_fallback_copy:
 * @self: a #ClawtModelFallback
 *
 * Returns: (transfer full): a deep copy
 */
ClawtModelFallback *clawt_model_fallback_copy(const ClawtModelFallback *self);

/**
 * clawt_model_fallback_free:
 * @self: (nullable): a #ClawtModelFallback
 */
void clawt_model_fallback_free(ClawtModelFallback *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtModelFallback, clawt_model_fallback_free)

/**
 * clawt_model_fallback_label:
 * @self: a #ClawtModelFallback
 *
 * "codex-cli gpt-5.5", or "codex-cli gpt-5.5 as backup" when the entry
 * names an account.
 *
 * Returns: (transfer full): the label
 */
gchar *clawt_model_fallback_label(const ClawtModelFallback *self);

/**
 * clawt_model_fallback_parse_list:
 * @text: (nullable): the list as text
 * @error: return location for what was wrong with it
 *
 * Reads the one text spelling of a fallback list: entries separated by
 * commas, each `provider:model`, optionally followed by `@account`.
 *
 * |[
 * codex-cli:gpt-5.5@backup, claude-code:sonnet
 * ]|
 *
 * The provider ends at the first colon -- a provider name never holds
 * one and a model name may (`llama3:8b`) -- and the account starts at
 * the last `@`.  Blank text is the empty list.  An entry missing its
 * provider or its model is refused rather than skipped: dropping it
 * would save a list shorter than the one somebody wrote, and say so
 * nowhere.
 *
 * Returns: (transfer full) (element-type ClawtModelFallback) (nullable):
 *   the entries, or %NULL with @error set
 */
GPtrArray *clawt_model_fallback_parse_list(const gchar  *text,
                                           GError      **error);

/**
 * clawt_model_fallback_list_to_string:
 * @fallbacks: (nullable) (element-type ClawtModelFallback): the entries
 *
 * The inverse of clawt_model_fallback_parse_list(), tested as a round
 * trip: what `agent.show` reports in `settings` and what a client sends
 * back to `agent.set`.
 *
 * Returns: (transfer full): the text, empty for no entries
 */
gchar *clawt_model_fallback_list_to_string(GPtrArray *fallbacks);

/**
 * clawt_model_fallback_account_fits:
 * @account_provider: (nullable): an entry of `accounts:`'s `provider`
 * @provider: (nullable): the CLI a fallback entry runs
 *
 * Whether that account is a login for that CLI -- what a fallback
 * entry's account picker offers.  `account.list` answers "usable" for
 * the agent's own provider only, and an entry may run another CLI, so
 * the editor asks this per entry instead.
 *
 * Compared as written: accounts.provider is documented in the canonical
 * spelling, and normalising would fit "claude" -- the catalogue's HTTP
 * provider -- to a Claude Code login.  The daemon still decides when the
 * list is saved, so this keeps a login for another CLI off the picker
 * rather than refusing anything.
 *
 * Returns: %TRUE when an entry for @provider may name the account
 */
gboolean clawt_model_fallback_account_fits(const gchar *account_provider,
                                           const gchar *provider);

/* ── Saving a list without losing one ────────────────────────────── */

/**
 * ClawtModelSetting:
 * @key: an agent-relative key, as `agent.set` takes it
 * @value: the value to write; an empty string clears
 *
 * One `agent.set` of a model key.
 */
typedef struct {
    gchar *key;
    gchar *value;
} ClawtModelSetting;

#define CLAWT_TYPE_MODEL_SETTING (clawt_model_setting_get_type())

GType clawt_model_setting_get_type(void) G_GNUC_CONST;

/**
 * clawt_model_setting_new:
 * @key: the key
 * @value: (nullable): the value; %NULL is written as an empty string
 *
 * Returns: (transfer full): a new #ClawtModelSetting
 */
ClawtModelSetting *clawt_model_setting_new(const gchar *key,
                                           const gchar *value);

/**
 * clawt_model_setting_copy:
 * @self: a #ClawtModelSetting
 *
 * Returns: (transfer full): a deep copy
 */
ClawtModelSetting *clawt_model_setting_copy(const ClawtModelSetting *self);

/**
 * clawt_model_setting_free:
 * @self: (nullable): a #ClawtModelSetting
 */
void clawt_model_setting_free(ClawtModelSetting *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtModelSetting, clawt_model_setting_free)

/**
 * clawt_model_fallback_restore_plan:
 * @saved_list: (nullable): the `model.fallbacks` the agent had before the
 *   save, as text
 * @old_provider: (nullable): its `model.provider` before the save
 * @old_account: (nullable): its `model.account` before the save
 * @old_model: (nullable): its `model.model` before the save
 * @undo_backend: %FALSE for the first attempt, %TRUE for the second
 *
 * What a client sends to put a fallback list back after a save that
 * cleared it went wrong.
 *
 * A save that changes the provider or the account clears the stored list
 * first, because the daemon checks a backend against the list it would
 * fall back from -- and then writes the new list last.  When anything
 * after the clear is refused, the agent was left with no list at all
 * until somebody saved again.  So the client asks for two plans:
 *
 * - the first (@undo_backend %FALSE) writes @saved_list back onto
 *   whatever backend the agent now has.  Usually enough: the old list
 *   fits the new provider more often than not, and the backend change
 *   somebody asked for stands.
 * - if the first is refused, the second (@undo_backend %TRUE) puts the
 *   backend back -- account cleared, the old provider, the old account,
 *   the old model -- and then the list, which fitted that backend when
 *   it was saved.
 *
 * Each plan runs in order and stops at its first refusal.
 *
 * Returns: (transfer full) (element-type ClawtModelSetting): the plan,
 *   empty when @saved_list is empty and there is nothing to keep
 */
GPtrArray *clawt_model_fallback_restore_plan(const gchar *saved_list,
                                             const gchar *old_provider,
                                             const gchar *old_account,
                                             const gchar *old_model,
                                             gboolean     undo_backend);

/**
 * clawt_failover_reason_phrase:
 * @reason: (nullable): the `reason` an agent reported: `usage-limit`,
 *   `rate-limit`, `signed-out` or `unavailable`
 *
 * "hit its usage limit", "is rate limited", "is signed out", "is
 * unavailable" -- or "refused a turn" for anything else, so a reason a
 * newer libreclaw adds still makes a sentence.
 *
 * Returns: (transfer none): a static phrase
 */
const gchar *clawt_failover_reason_phrase(const gchar *reason);

/**
 * clawt_failover_is_active:
 * @failover: (nullable): an agent's `failover` object, as `agent.list`
 *   and `agent.show` report it
 *
 * Whether the agent is running on a fallback.  The question a client's
 * badge asks, asked in one place.
 *
 * Returns: %TRUE when @failover says `state: fallback`
 */
gboolean clawt_failover_is_active(JsonObject *failover);

/**
 * clawt_failover_badge:
 * @failover: (nullable): an agent's `failover` object
 *
 * The short text for a badge: "On fallback: codex-cli gpt-5.5".
 *
 * Returns: (transfer full) (nullable): the text, or %NULL when the agent
 *   is not on a fallback
 */
gchar *clawt_failover_badge(JsonObject *failover);

/**
 * clawt_failover_describe:
 * @agent: (nullable): the agent's name or id
 * @failover: (nullable): an agent's `failover` object
 *
 * The sentence for an alert, a tooltip and `agent show`: "researcher is
 * running on fallback codex-cli gpt-5.5 because claude-code opus hit its
 * usage limit, back at 18:50 (it starts a fresh conversation there: the
 * context did not come along)".  When the agent is back on its primary:
 * "researcher is back on claude-code opus".
 *
 * Times are the local clock, without a date when they fall today.  A
 * reset the provider did not name is "trying again at", because a
 * backoff is a guess and must not read like a promise.
 *
 * Returns: (transfer full) (nullable): the sentence, or %NULL for no
 *   @failover
 */
gchar *clawt_failover_describe(const gchar *agent,
                               JsonObject  *failover);

G_END_DECLS
