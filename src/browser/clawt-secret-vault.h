/*
 * clawt-secret-vault.h - One-use references to secrets an agent never sees
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * An agent that has to log in names a `vault:` entry and gets back a
 * reference -- `secret-ref:` and 32 hex digits from the kernel's pool --
 * which it hands to clawtilla_browser_fill_secret.  The reference is the
 * only thing the agent ever holds:
 *
 * - it names an entry, not a value: the value is resolved at the fill,
 *   on a worker, and typed into the page by gsurf;
 * - it works once.  The first redemption spends it, whoever makes it, so
 *   a reference that leaks into a transcript is already worthless;
 * - it is bound to the agent and the conversation it was asked for in.
 *   A reference carried into another room, or quoted to another agent,
 *   is refused there -- and spent, since an attempt from the wrong place
 *   is the one sign that it went somewhere it should not have;
 * - it expires (%CLAWT_SECRET_REF_TTL_SECONDS) and does not survive a
 *   daemon restart.
 *
 * Nothing here holds or resolves a secret.  The table maps references to
 * entry names, so even a dump of this object would show no value.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * CLAWT_SECRET_REF_TTL_SECONDS:
 *
 * How long a reference lasts unused: long enough for the agent to find
 * the field, short enough that one it never used is soon gone.
 */
#define CLAWT_SECRET_REF_TTL_SECONDS (600)

/**
 * CLAWT_SECRET_REF_PREFIX:
 *
 * What every reference starts with, so a person reading a transcript can
 * tell one from a secret at a glance.
 */
#define CLAWT_SECRET_REF_PREFIX "secret-ref:"

#define CLAWT_TYPE_SECRET_VAULT (clawt_secret_vault_get_type())

G_DECLARE_FINAL_TYPE(ClawtSecretVault, clawt_secret_vault, CLAWT, SECRET_VAULT, GObject)

/**
 * clawt_secret_vault_new:
 *
 * Returns: (transfer full): an empty table of references
 */
ClawtSecretVault *clawt_secret_vault_new(void);

/**
 * clawt_secret_vault_mint:
 * @self: a #ClawtSecretVault
 * @agent_id: the agent asking
 * @room_id: (nullable): the conversation it is asking in; %NULL is its
 *   own unnamed one, which only another %NULL matches
 * @name: the `vault:` entry the reference stands for
 * @now: the current time, in seconds since the epoch
 * @error: (out) (optional): return location for a #GError
 *
 * A new one-use reference to @name.  Whether @agent_id may have @name is
 * the caller's question, asked before this; the reference records only
 * whom it was given to.
 *
 * Returns: (transfer full) (nullable): the reference, or %NULL when the
 *   kernel's random pool could not be read
 */
gchar *clawt_secret_vault_mint(ClawtSecretVault  *self,
                               const gchar       *agent_id,
                               const gchar       *room_id,
                               const gchar       *name,
                               gint64             now,
                               GError           **error);

/**
 * clawt_secret_vault_redeem:
 * @self: a #ClawtSecretVault
 * @ref: the reference the agent handed back
 * @agent_id: the agent handing it back
 * @room_id: (nullable): the conversation it is calling from
 * @now: the current time, in seconds since the epoch
 * @error: (out) (optional): return location for a #GError
 *
 * Spends @ref and answers the entry name it stands for.  Refused, with a
 * sentence the agent can act on, when @ref is unknown, already used,
 * expired, or was given to another agent or conversation; every refusal
 * but "unknown" also spends it.
 *
 * Returns: (transfer full) (nullable): the entry name, or %NULL
 */
gchar *clawt_secret_vault_redeem(ClawtSecretVault  *self,
                                 const gchar       *ref,
                                 const gchar       *agent_id,
                                 const gchar       *room_id,
                                 gint64             now,
                                 GError           **error);

/**
 * clawt_secret_vault_count_live:
 * @self: a #ClawtSecretVault
 * @now: the current time, in seconds since the epoch
 *
 * Returns: how many references are still unspent and unexpired
 */
guint clawt_secret_vault_count_live(ClawtSecretVault *self, gint64 now);

/**
 * clawt_secret_vault_forget_agent:
 * @self: a #ClawtSecretVault
 * @agent_id: an agent
 *
 * Drops every reference given to @agent_id -- when it stops, or loses
 * the grant a reference was minted under.
 */
void clawt_secret_vault_forget_agent(ClawtSecretVault *self, const gchar *agent_id);

G_END_DECLS
